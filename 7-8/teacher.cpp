#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <ctime>
#include <sys/mman.h>
#include <semaphore.h>
#include "common.h"

using namespace std;

SharedData *shm = nullptr;
int shm_fd = -1;
sem_t *mutex_sem = nullptr;
sem_t *queue_sem = nullptr;

volatile sig_atomic_t running = 1;

// уведомление всех студентов
void notify_all_students() {
    cout << "[Teacher] Notifying all students about shutdown...\n";

    sem_wait(mutex_sem);
    shm->shutdown = true;

    for (int i = 0; i < shm->capacity; i++) {
        if (shm->slots[i].state == SLOT_WAITING ||
            shm->slots[i].state == SLOT_PROCESSING)
        {
            shm->slots[i].grade = -1;  // специальная "оценка"
            sem_t *grade = sem_open(shm->slots[i].grade_sem_name, 0);
            if (grade != SEM_FAILED) {
                sem_post(grade);
                sem_close(grade);
                cout << "[Teacher] Notified PID=" << shm->slots[i].pid << endl;
            }
        }
    }

    sem_post(mutex_sem);
}

void handle_sigint(int) {
    cout << "\n[Teacher] SIGINT received. Finishing...\n";
    running = 0;
    notify_all_students();
    sem_post(queue_sem); // разбудить teacher, если он ждёт
}

void cleanup() {
    cout << "[Teacher] Cleaning resources...\n";

    if (mutex_sem) { sem_close(mutex_sem); sem_unlink(MUTEX_NAME); }
    if (queue_sem) { sem_close(queue_sem); sem_unlink(QUEUE_NAME); }

    if (shm) munmap(shm, sizeof(SharedData));
    if (shm_fd >= 0) { close(shm_fd); shm_unlink(SHM_NAME); }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        cerr << "Usage: ./teacher <capacity>\n";
        return 1;
    }

    int capacity = atoi(argv[1]);
    // 1024 - ограничение чтобы не улететь
    if (capacity <= 0 || capacity > 1024) {
        cerr << "Capacity must be 1.." << 1024 << endl;
        return 1;
    }

    signal(SIGINT, handle_sigint);
    srand(time(nullptr));

    size_t total_size = capacity * sizeof(StudentSlot) + sizeof(SharedData);

    shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, total_size);
    shm = static_cast<SharedData *>(mmap(nullptr, total_size,
                                         PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0));

    shm->capacity = capacity;
    shm->shutdown = false;
    shm->active_students = 0;

    for (int i = 0; i < capacity; i++)
        shm->slots[i].state = SLOT_EMPTY;

    sem_unlink(MUTEX_NAME);
    sem_unlink(QUEUE_NAME);

    mutex_sem = sem_open(MUTEX_NAME, O_CREAT | O_EXCL, 0666, 1);
    queue_sem = sem_open(QUEUE_NAME, O_CREAT | O_EXCL, 0666, 0);

    cout << "[Teacher] Ready. Capacity = " << capacity << endl;

    while (running) {
        // ждём нового студента
        sem_wait(queue_sem);
        if (!running) break;

        // находим первого ожидающего
        sem_wait(mutex_sem);
        int idx = -1;

        for (int i = 0; i < capacity; i++)
            if (shm->slots[i].state == SLOT_WAITING) {
                idx = i;
                shm->slots[i].state = SLOT_PROCESSING;
                break;
            }

        sem_post(mutex_sem);
        if (idx == -1) continue;

        StudentSlot &s = shm->slots[idx];

        cout << "[Teacher] Checking PID=" << s.pid
             << " ticket=" << s.ticket << endl;

        sleep(1 + rand() % 3);
        s.grade = 3 + rand() % 3;

        sem_t *grade = sem_open(s.grade_sem_name, 0);
        sem_t *ack   = sem_open(s.ack_sem_name, 0);

        sem_post(grade);
        sem_wait(ack);

        sem_close(grade);
        sem_close(ack);

        cout << "[Teacher] Grade=" << s.grade
             << " PID=" << s.pid << endl;

        sem_wait(mutex_sem);
        s.state = SLOT_EMPTY;
        shm->active_students--;
        sem_post(mutex_sem);
    }

    cleanup();
    return 0;
}