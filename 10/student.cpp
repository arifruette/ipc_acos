#include <iostream>
#include <cstring>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

#include "common.h"

using namespace std;

SharedData *shm = nullptr;
int shm_fd = -1;
sem_t *mutex_sem = nullptr;
sem_t *queue_sem = nullptr;

sem_t *grade_sem = nullptr;
sem_t *ack_sem = nullptr;

char grade_name[64];
char ack_name[64];

volatile sig_atomic_t interrupted = 0;

void handle_sigint(int) { interrupted = 1; }

void print_local(const string &s) { cout << s << endl; }

void send_fifo_one(const string &s) {
    int fd = open(FIFO_NAME, O_WRONLY | O_NONBLOCK);
    if (fd >= 0) {
        write(fd, s.c_str(), s.size());
        close(fd);
    }
}

void log_both(const string &who, const string &msg) {
    string s = "[" + who + "] " + msg + "\n";
    print_local(s);
    send_fifo_one(s);
}

void cleanup() {
    if (grade_sem) { sem_close(grade_sem); sem_unlink(grade_name); grade_sem = nullptr; }
    if (ack_sem)   { sem_close(ack_sem);   sem_unlink(ack_name);   ack_sem = nullptr; }
    if (mutex_sem) { sem_close(mutex_sem); mutex_sem = nullptr; }
    if (queue_sem) { sem_close(queue_sem); queue_sem = nullptr; }
    if (shm) { munmap(shm, sizeof(SharedData)); shm = nullptr; }
    if (shm_fd >= 0) { close(shm_fd); shm_fd = -1; }
}

int main() {
    signal(SIGINT, handle_sigint);

    pid_t pid = getpid();
    srand((unsigned)time(nullptr) ^ pid);

    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) {
        cout << "[STUDENT " << pid << "] Teacher not running.\n";
        return 0;
    }

    // получаем размер
    struct stat st;
    if (fstat(shm_fd, &st) < 0) {
        perror("fstat");
        close(shm_fd);
        return 1;
    }
    size_t map_size = st.st_size;
    if (map_size < sizeof(SharedData)) {
        cerr << "[STUDENT " << pid << "] shared memory too small\n";
        close(shm_fd);
        return 1;
    }

    shm = static_cast<SharedData *>(mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));
    if (shm == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return 1;
    }

    mutex_sem = sem_open(MUTEX_NAME, 0);
    queue_sem = sem_open(QUEUE_NAME, 0);
    if (mutex_sem == SEM_FAILED || queue_sem == SEM_FAILED) {
        perror("sem_open");
        cleanup();
        return 1;
    }

    sprintf(grade_name, "/grade_%d", pid);
    sprintf(ack_name,   "/ack_%d",   pid);

    grade_sem = sem_open(grade_name, O_CREAT, 0666, 0);
    ack_sem   = sem_open(ack_name,   O_CREAT, 0666, 0);
    if (grade_sem == SEM_FAILED || ack_sem == SEM_FAILED) {
        perror("sem_open per-student");
        cleanup();
        return 1;
    }

    int ticket = 1 + rand() % 100;
    int prep = 1 + rand() % 3;

    log_both("STUDENT " + to_string(pid), "Preparing " + to_string(prep) + "s, ticket=" + to_string(ticket));
    sleep(prep);

    if (interrupted || shm->shutdown) {
        log_both("STUDENT " + to_string(pid), "Interrupted during preparation");
        cleanup();
        return 0;
    }

    int slot = -1;

    sem_wait(mutex_sem);
    if (!shm->shutdown) {
        for (int i = 0; i < shm->capacity; ++i) {
            if (shm->slots[i].state == SLOT_EMPTY) {
                slot = i;
                shm->slots[i].state = SLOT_WAITING;
                shm->slots[i].pid = pid;
                shm->slots[i].ticket = ticket;
                strncpy(shm->slots[i].grade_sem_name, grade_name, sizeof(shm->slots[i].grade_sem_name)-1);
                strncpy(shm->slots[i].ack_sem_name,   ack_name,   sizeof(shm->slots[i].ack_sem_name)-1);
                shm->active_students++;
                break;
            }
        }
    }
    sem_post(mutex_sem);

    if (slot == -1) {
        log_both("STUDENT " + to_string(pid), "No free slots, leaving");
        cleanup();
        return 0;
    }log_both("STUDENT " + to_string(pid), "Registered in slot " + to_string(slot));
    sem_post(queue_sem);

    bool received = false;
    while (!interrupted) {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        int r = sem_timedwait(grade_sem, &ts);
        if (r == 0) {
            received = true;
            break;
        }
        if (shm->shutdown) break;
    }

    if (!received) {
        log_both("STUDENT " + to_string(pid), "Exam ended before receiving grade");
        sem_wait(mutex_sem);
        shm->slots[slot].state = SLOT_EMPTY;
        shm->active_students--;
        sem_post(mutex_sem);
        cleanup();
        return 0;
    }

    int grade = shm->slots[slot].grade;
    log_both("STUDENT " + to_string(pid), "Received grade: " + to_string(grade));

    sem_post(ack_sem);

    sem_wait(mutex_sem);
    shm->slots[slot].state = SLOT_EMPTY;
    shm->active_students--;
    sem_post(mutex_sem);

    cleanup();
    return 0;
}