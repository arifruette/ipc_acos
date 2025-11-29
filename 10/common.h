#ifndef COMMON_H
#define COMMON_H

static const char *SHM_NAME   = "/exam_shm";
static const char *MUTEX_NAME = "/exam_mutex";
static const char *QUEUE_NAME = "/exam_queue";
static const char *FIFO_NAME  = "/tmp/exam_log";

enum SlotState {
    SLOT_EMPTY = 0,
    SLOT_WAITING,
    SLOT_PROCESSING,
    SLOT_DONE,
    SLOT_ERROR
};

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

#endif // COMMON_H