#include <iostream>
#include <cstring>
#include <unistd.h>
#include <csignal>
#include <fcntl.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <ctime>
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

void handle_sigint(int) {
    interrupted = 1;
}

void cleanup() {
    if (grade_sem) {
        sem_close(grade_sem);
        sem_unlink(grade_name);
    }
    if (ack_sem) {
        sem_close(ack_sem);
        sem_unlink(ack_name);
    }
    if (mutex_sem) sem_close(mutex_sem);
    if (queue_sem) sem_close(queue_sem);

    if (shm) munmap(shm, sizeof(SharedData));
    if (shm_fd >= 0) close(shm_fd);
}

int main() {
    signal(SIGINT, handle_sigint);

    pid_t pid = getpid();
    srand(time(nullptr) ^ pid);

    // Подключение к shared memory
    shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) {
        cerr << "[Student " << pid << "] Teacher not running\n";
        return 0;
    }

    shm = static_cast<SharedData *>(mmap(nullptr, sizeof(SharedData),
                                         PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));

    mutex_sem = sem_open(MUTEX_NAME, 0);
    queue_sem = sem_open(QUEUE_NAME, 0);

    sprintf(grade_name, "/grade_%d", pid);
    sprintf(ack_name,   "/ack_%d",   pid);

    grade_sem = sem_open(grade_name, O_CREAT, 0666, 0);
    ack_sem   = sem_open(ack_name,   O_CREAT, 0666, 0);

    int ticket = 1 + rand() % 100;
    int prep_time = 1 + rand() % 3;

    cout << "[Student " << pid << "] Preparing " << prep_time
         << "s, ticket=" << ticket << endl;

    // Подготовка
    sleep(prep_time);

    if (interrupted || shm->shutdown) {
        cout << "[Student " << pid << "] Interrupted during preparation.\n";
        cleanup();
        return 0;
    }

    // пытаемся занять слот
    int slot = -1;

    sem_wait(mutex_sem);

    if (shm->shutdown) {
        sem_post(mutex_sem);
        cout << "[Student " << pid << "] Exam closed before registration.\n";
        cleanup();
        return 0;
    }

    for (int i = 0; i < shm->capacity; i++) {
        if (shm->slots[i].state == SLOT_EMPTY) {
            slot = i;
            shm->slots[i].state = SLOT_WAITING;
            shm->slots[i].pid = pid;
            shm->slots[i].ticket = ticket;

            strcpy(shm->slots[i].grade_sem_name, grade_name);
            strcpy(shm->slots[i].ack_sem_name,   ack_name);

            shm->active_students++;
            break;
        }
    }

    sem_post(mutex_sem);

    if (slot == -1) {
        cout << "[Student " << pid << "] No free slots. Exiting...\n";
        cleanup();
        return 0;
    }

    // Уведомляем преподавателя
    sem_post(queue_sem);

    // ждем оценку
    bool grade_received = false;

    while (!interrupted) {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;

        int r = sem_timedwait(grade_sem, &ts);

        if (r == 0) {
            grade_received = true;
            break;
        }
        if (shm->shutdown) break;
    }

    if (!grade_received) {
        cout << "[Student " << pid << "] Exam ended before grading.\n";

        // освобождаем слот
        sem_wait(mutex_sem);
        shm->slots[slot].state = SLOT_EMPTY;
        shm->active_students--;
        sem_post(mutex_sem);

        cleanup();
        return 0;
    }

    int grade = shm->slots[slot].grade;

    if (grade == -1)
        cout << "[Student " << pid << "] Teacher shutdown.\n";
    else
        cout << "[Student " << pid << "] Grade received: " << grade << endl;

    // отправляем подтверждение
    sem_post(ack_sem);

    // освобождаем слот
    sem_wait(mutex_sem);
    shm->slots[slot].state = SLOT_DONE;
    shm->active_students--;
    shm->slots[slot].state = SLOT_EMPTY;
    sem_post(mutex_sem);

    cleanup();
    return 0;
}