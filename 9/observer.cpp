#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <csignal>
#include <cstring>
#include <cerrno>
#include <sys/stat.h>

#include "common.h"

using namespace std;

volatile sig_atomic_t running = 1;
void handle_sigint(int) { running = 0; }

int main() {
    signal(SIGINT, handle_sigint);

    // ensure FIFO exists
    if (mkfifo(FIFO_NAME, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        return 1;
    }

    cout << "[Observer] Started. Waiting for logs...\n";

    // open for reading non-blocking
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
            cout << buf;
            cout.flush();
            continue;
        }
        if (n == -1 && errno == EAGAIN) {
            // no data, sleep shortly
            usleep(100000);
            continue;
        }
        if (n == 0) {
            // all writers closed -> reopen to wait for new writers
            close(fd);
            fd = open(FIFO_NAME, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                // pipe disappeared
                break;
            }
            continue;
        }
    }

    cout << "\n[Observer] Shutdown.\n";
    close(fd);
    // do not unlink here to allow restarts; if you want cleanup, uncomment:
    // unlink(FIFO_NAME);
    return 0;
}