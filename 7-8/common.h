#ifndef COMMON_H
#define COMMON_H

static const char *SHM_NAME   = "/exam_shm";
static const char *MUTEX_NAME = "/exam_mutex";
static const char *QUEUE_NAME = "/exam_queue";

// Состояния слота
enum SlotState {
    SLOT_EMPTY = 0,        // свободно
    SLOT_WAITING = 1,      // студент ожидает проверки
    SLOT_PROCESSING = 2,   // преподаватель проверяет
    SLOT_DONE = 3,         // студент завершён нормально
    SLOT_ERROR = 4         // аварийный выход
};

// Структуры

struct StudentSlot {
    pid_t pid;
    int ticket;
    int grade;
    SlotState state;

    char grade_sem_name[64];
    char ack_sem_name[64];
};

struct SharedData {
    int capacity;
    bool shutdown;
    int active_students;

    StudentSlot slots[];
};

#endif