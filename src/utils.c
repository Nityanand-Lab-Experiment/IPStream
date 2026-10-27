#define _POSIX_C_SOURCE 200809L
#include "utils.h"

#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <string.h>

int64_t get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000LL);
}

void format_bytes(uint64_t bytes, char *buf, size_t buf_len) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(buf, buf_len, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(buf, buf_len, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else if (bytes >= 1024ULL) {
        snprintf(buf, buf_len, "%.2f KB", (double)bytes / 1024.0);
    } else {
        snprintf(buf, buf_len, "%llu B", (unsigned long long)bytes);
    }
}

void format_time(int64_t seconds, char *buf, size_t buf_len) {
    int64_t hrs = seconds / 3600;
    int64_t mins = (seconds % 3600) / 60;
    int64_t secs = seconds % 60;
    snprintf(buf, buf_len, "%02lld:%02lld:%02lld", (long long)hrs, (long long)mins, (long long)secs);
}

void setup_signal_handlers(void (*handler)(int)) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    /* Ignore SIGPIPE so broken network sockets do not kill the process */
    signal(SIGPIPE, SIG_IGN);
}
