#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <ctime>
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/stat.h>
#include <cerrno>

#include "common.h"

using namespace std;

SharedData *shm = nullptr;
int shm_fd = -1;
sem_t *mutex_sem = nullptr;
sem_t *queue_sem = nullptr;
int fifo_fd = -1;
size_t shm_size = 0;

volatile sig_atomic_t running = 1;

void print_local(const string &s) {
    cout << s << endl;
}

void send_fifo(const string &msg) {
    if (fifo_fd >= 0) {
        ssize_t w = write(fifo_fd, msg.c_str(), msg.size());
        if (w == (ssize_t)msg.size()) return;
    }
    int fd = open(FIFO_NAME, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        write(fd, msg.c_str(), msg.size());
        close(fd);
    }
}

void log_msg_both(const string &who, const string &msg) {
    string s = "[" + who + "] " + msg + "\n";
    print_local(s);
    send_fifo(s);
}

void notify_all_students() {
    log_msg_both("TEACHER", "Shutdown: notifying all students");
    sem_wait(mutex_sem);
    shm->shutdown = true;

    for (int i = 0; i < shm->capacity; ++i) {
        if (shm->slots[i].state == SLOT_WAITING ||
            shm->slots[i].state == SLOT_PROCESSING) {

            shm->slots[i].grade = -1;
            sem_t *g = sem_open(shm->slots[i].grade_sem_name, 0);
            if (g != SEM_FAILED) {
                sem_post(g);
                sem_close(g);
            }
        }
    }
    sem_post(mutex_sem);
}

void handle_sigint(int) {
    running = 0;
    log_msg_both("TEACHER", "SIGINT received, finishing...");
    notify_all_students();
    if (queue_sem) sem_post(queue_sem);
}

void cleanup() {
    log_msg_both("TEACHER", "Cleaning resources");
    if (mutex_sem) { sem_close(mutex_sem); sem_unlink(MUTEX_NAME); mutex_sem = nullptr; }
    if (queue_sem) { sem_close(queue_sem); sem_unlink(QUEUE_NAME); queue_sem = nullptr; }

    if (shm) {
        munmap(shm, shm_size);
        shm = nullptr;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_unlink(SHM_NAME);
        shm_fd = -1;
    }

    if (fifo_fd >= 0) {
        close(fifo_fd);
        fifo_fd = -1;
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        cerr << "Usage: ./teacher <capacity>\n";
        return 1;
    }

    int capacity = atoi(argv[1]);
    if (capacity <= 0 || capacity > 1024) {
        cerr << "Capacity must be 1..1024\n";
        return 1;
    }

    signal(SIGINT, handle_sigint);
    srand((unsigned)time(nullptr));

    if (mkfifo(FIFO_NAME, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
    } else {
        fifo_fd = open(FIFO_NAME, O_WRONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            fifo_fd = -1;
        }
    }

    shm_size = sizeof(SharedData) + capacity * sizeof(StudentSlot);
    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }
    if (ftruncate(shm_fd, shm_size) < 0) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }

    shm = static_cast<SharedData *>(mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (shm == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        shm_unlink(SHM_NAME);
        return 1;
    }// init shared data
    shm->capacity = capacity;
    shm->shutdown = false;
    shm->active_students = 0;
    for (int i = 0; i < capacity; ++i) {
        shm->slots[i].state = SLOT_EMPTY;
        shm->slots[i].pid = 0;
        shm->slots[i].ticket = 0;
        shm->slots[i].grade = 0;
        shm->slots[i].grade_sem_name[0] = '\0';
        shm->slots[i].ack_sem_name[0] = '\0';
    }

    sem_unlink(MUTEX_NAME);
    sem_unlink(QUEUE_NAME);

    mutex_sem = sem_open(MUTEX_NAME, O_CREAT, 0666, 1);
    queue_sem = sem_open(QUEUE_NAME, O_CREAT, 0666, 0);
    if (mutex_sem == SEM_FAILED || queue_sem == SEM_FAILED) {
        perror("sem_open");
        cleanup();
        return 1;
    }

    log_msg_both("TEACHER", "Ready. Capacity=" + to_string(capacity));

    while (running) {
        if (sem_wait(queue_sem) == -1) {
            if (errno == EINTR && !running) break;
            continue;
        }
        if (!running) break;

        sem_wait(mutex_sem);
        int idx = -1;
        for (int i = 0; i < capacity; ++i) {
            if (shm->slots[i].state == SLOT_WAITING) {
                idx = i;
                shm->slots[i].state = SLOT_PROCESSING;
                break;
            }
        }
        sem_post(mutex_sem);

        if (idx == -1) continue;

        StudentSlot &s = shm->slots[idx];

        log_msg_both("TEACHER", "Checking PID=" + to_string(s.pid) + " ticket=" + to_string(s.ticket));

        sleep(1 + rand() % 3);
        s.grade = 3 + rand() % 3;

        sem_t *grade = sem_open(s.grade_sem_name, 0);
        sem_t *ack   = sem_open(s.ack_sem_name, 0);

        if (grade == SEM_FAILED || ack == SEM_FAILED) {
            log_msg_both("TEACHER", "Failed to open per-student semaphores for PID=" + to_string(s.pid));
            if (grade != SEM_FAILED) sem_close(grade);
            if (ack != SEM_FAILED) sem_close(ack);

            sem_wait(mutex_sem);
            s.state = SLOT_EMPTY;
            shm->active_students--;
            sem_post(mutex_sem);
            continue;
        }

        sem_post(grade);

        while (true) {
            if (sem_wait(ack) == -1) {
                if (errno == EINTR) {
                    if (!running) break;
                    else continue;
                } else break;
            } else break;
        }

        sem_close(grade);
        sem_close(ack);

        log_msg_both("TEACHER", "Grade=" + to_string(s.grade) + " PID=" + to_string(s.pid));

        sem_wait(mutex_sem);
        s.state = SLOT_EMPTY;
        shm->active_students--;
        sem_post(mutex_sem);
    }

    log_msg_both("TEACHER", "Exiting.");
    cleanup();
    return 0;
}