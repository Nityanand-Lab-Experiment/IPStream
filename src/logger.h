#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3
} LogLevel;

#ifdef __cplusplus
extern "C" {
#endif

void log_set_level(LogLevel level);
void log_msg(LogLevel level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

/* Ensure conflicting syslog macros (from glibc syslog.h or srt logging_api.h) are replaced */
#ifdef LOG_DEBUG
#undef LOG_DEBUG
#endif
#define LOG_DEBUG(fmt, ...) log_msg(LOG_LVL_DEBUG, fmt, ##__VA_ARGS__)

#ifdef LOG_INFO
#undef LOG_INFO
#endif
#define LOG_INFO(fmt, ...)  log_msg(LOG_LVL_INFO,  fmt, ##__VA_ARGS__)

#ifdef LOG_WARN
#undef LOG_WARN
#endif
#define LOG_WARN(fmt, ...)  log_msg(LOG_LVL_WARN,  fmt, ##__VA_ARGS__)

#ifdef LOG_ERROR
#undef LOG_ERROR
#endif
#define LOG_ERROR(fmt, ...) log_msg(LOG_LVL_ERROR, fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */
