#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <sys/stat.h>

#include "common.h"

using namespace std;

volatile sig_atomic_t running = 1;
void handle_sigint(int) { running = 0; }

int main() {
    signal(SIGINT, handle_sigint);

    pid_t pid = getpid();

    if (mkfifo(FIFO_NAME, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        return 1;
    }

    cout << "[Observer " << pid << "] Started. Waiting for logs...\n";

    int fd = open(FIFO_NAME, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("open fifo");
        return 1;
    }

    char buf[512];

    while (running) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            cout << "[Observer " << pid << "] " << buf;
            cout.flush();
            continue;
        }
        if (n == -1 && errno == EAGAIN) {
            usleep(100000);
            continue;
        }
        if (n == 0) {
            close(fd);
            fd = open(FIFO_NAME, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                break;
            }
            continue;
        }
    }

    cout << "\n[Observer " << pid << "] Shutdown.\n";
    close(fd);
    // FIFO не удаляем, чтобы можно было перезапускать наблюдателей/teacher
    return 0;
}
