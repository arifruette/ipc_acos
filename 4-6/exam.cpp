#include <iostream>
#include <unistd.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <csignal>
#include <sys/wait.h>
#include <random>

using namespace std;

// Структура заголовка: статичная часть разделяемой памяти
struct SharedHeader {
    sem_t mutex; // защита таблиц
    sem_t queue; // количество готовых студентов
    bool running; // флаг работы
    int N; // количество студентов
};

// Глобальные указатели на массивы в shared memory
SharedHeader *hdr = nullptr;
sem_t *grade_sem = nullptr; // преподаватель -> студент
sem_t *ack_sem = nullptr; // студент -> преподаватель (подтверждение)
int *tickets_arr = nullptr;
int *grades_arr = nullptr;
int *state_arr = nullptr; // 0 не готов, 1 ждет проверки, 2 проверен
pid_t *child_pids = nullptr;

void cleanup() {
    if (!hdr) return;

    sem_destroy(&hdr->mutex);
    sem_destroy(&hdr->queue);

    for (int i = 0; i < hdr->N; i++) {
        sem_destroy(&grade_sem[i]);
        sem_destroy(&ack_sem[i]);
    }

    size_t size =
            sizeof(SharedHeader)
            + hdr->N * sizeof(sem_t) * 2
            + hdr->N * sizeof(int) * 3
            + hdr->N * sizeof(pid_t);

    munmap(hdr, size);
    hdr = nullptr;
    cout << "Done all cleanup";
}

void on_sigint(int) {
    if (!hdr) return;

    hdr->running = false;
    sem_post(&hdr->queue);

    for (int i = 0; i < hdr->N; i++) {
        sem_post(&grade_sem[i]);
        sem_post(&ack_sem[i]);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        cerr << "Usage: ./exam <num_students>\n";
        return 1;
    }

    int N = atoi(argv[1]);
    // ограничение сверху чисто чтобы железка не отлетела
    if (N <= 0 || N > 10000) {
        cerr << "Number of students must be > 0\n && <= 10000";
        return 1;
    }

    signal(SIGINT, on_sigint);

    // выделяем память
    size_t size =
            sizeof(SharedHeader)
            + N * sizeof(sem_t) * 2
            + N * sizeof(int) * 3
            + N * sizeof(pid_t);

    void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    hdr = static_cast<SharedHeader *>(mem);

    void *ptr = static_cast<char *>(mem) + sizeof(SharedHeader);
    grade_sem = static_cast<sem_t *>(ptr);

    ptr = static_cast<char *>(ptr) + N * sizeof(sem_t);
    ack_sem = static_cast<sem_t *>(ptr);

    ptr = static_cast<char *>(ptr) + N * sizeof(sem_t);
    tickets_arr = static_cast<int *>(ptr);

    ptr = static_cast<char *>(ptr) + N * sizeof(int);
    grades_arr = static_cast<int *>(ptr);

    ptr = static_cast<char *>(ptr) + N * sizeof(int);
    state_arr = static_cast<int *>(ptr);

    ptr = static_cast<char *>(ptr) + N * sizeof(int);
    child_pids = static_cast<pid_t *>(ptr);

    // инициализируем общую память
    hdr->N = N;
    hdr->running = true;

    sem_init(&hdr->mutex, 1, 1);
    sem_init(&hdr->queue, 1, 0);

    for (int i = 0; i < N; i++) {
        sem_init(&grade_sem[i], 1, 0);
        sem_init(&ack_sem[i], 1, 0);
        tickets_arr[i] = 0;
        grades_arr[i] = 0;
        state_arr[i] = 0;
    }

    cout << "[Teacher] Exam started with " << N << " students.\n";
    // fork процессов студентов
    for (int i = 0; i < N; i++) {
        pid_t pid = fork();

        if (pid == 0) {
            // процесс студента
            random_device rd;
            mt19937 rng(rd() ^ getpid() << 1);

            uniform_int_distribution<int> ticket_dist(1, 100);
            uniform_int_distribution<int> prep_dist(1, 4);

            int idx = i;

            int ticket = ticket_dist(rng);
            int prep_time = prep_dist(rng);

            cout << "[Student " << idx << "] Ticket " << ticket
                    << ", preparing for " << prep_time << "s\n";

            sleep(prep_time);

            sem_wait(&hdr->mutex);
            tickets_arr[idx] = ticket;
            state_arr[idx] = 1;
            sem_post(&hdr->mutex);

            sem_post(&hdr->queue);

            // ждем оценку от препода
            sem_wait(&grade_sem[idx]);

            if (!hdr->running) exit(0);

            cout << "[Student " << idx << "] Got grade "
                    << grades_arr[idx] << "\n";

            // Оповещение преподавателя, что вывод завершён
            sem_post(&ack_sem[idx]);

            exit(0);
        }

        child_pids[i] = pid;
    }

    // код учителя
    int processed = 0;

    mt19937 rng(random_device{}() ^ getpid() << 1);
    uniform_int_distribution<int> grade_dist(3, 5);
    uniform_int_distribution<int> check_dist(1, 3);

    while (processed < N && hdr->running) {
        sem_wait(&hdr->queue);
        if (!hdr->running) break;

        sem_wait(&hdr->mutex);
        int idx = -1;
        for (int i = 0; i < N; i++)
            if (state_arr[i] == 1) {
                idx = i;
                break;
            }
        if (idx != -1) state_arr[idx] = 2;
        sem_post(&hdr->mutex);

        if (idx == -1) continue;

        int ticket = tickets_arr[idx];

        cout << "[Teacher] Checking student " << idx
                << " (ticket " << ticket << ")\n";

        sleep(check_dist(rng));

        int grade = grade_dist(rng);

        sem_wait(&hdr->mutex);
        grades_arr[idx] = grade;
        sem_post(&hdr->mutex);

        cout << "[Teacher] Gave grade " << grade
                << " to student " << idx << "\n";

        // Отдать оценку
        sem_post(&grade_sem[idx]);

        // Ждать подтверждения от студента
        sem_wait(&ack_sem[idx]);

        processed++;
    }

    cout << "[Teacher] All students processed. Waiting children...\n";

    for (int i = 0; i < N; i++)
        waitpid(child_pids[i], nullptr, 0);

    cleanup();
    return 0;
}
