#define _POSIX_C_SOURCE 200809L
#include "utils.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <string.h>

static LogLevel s_current_level = LOG_LVL_INFO;

void log_set_level(LogLevel level) {
    s_current_level = level;
}

void log_msg(LogLevel level, const char *fmt, ...) {
    if (level < s_current_level) {
        return;
    }

    const char *color = "";
    const char *reset = "\033[0m";
    const char *prefix = "";

    switch (level) {
        case LOG_LVL_DEBUG:
            color = "\033[36m"; /* Cyan */
            prefix = "[DEBUG]";
            break;
        case LOG_LVL_INFO:
            color = "\033[32m"; /* Green */
            prefix = "[INFO] ";
            break;
        case LOG_LVL_WARN:
            color = "\033[33m"; /* Yellow */
            prefix = "[WARN] ";
            break;
        case LOG_LVL_ERROR:
            color = "\033[31m"; /* Red */
            prefix = "[ERROR]";
            break;
    }

    time_t rawtime;
    struct tm timeinfo;
    char time_str[32];

    time(&rawtime);
    localtime_r(&rawtime, &timeinfo);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

    FILE *out = (level >= LOG_LVL_WARN) ? stderr : stdout;

    fprintf(out, "%s %s%s%s ", time_str, color, prefix, reset);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);
}

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
