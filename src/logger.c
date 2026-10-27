#define _POSIX_C_SOURCE 200809L
#include "logger.h"

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
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
