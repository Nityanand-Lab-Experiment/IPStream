#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3
} LogLevel;

void log_set_level(LogLevel level);
void log_msg(LogLevel level, const char *fmt, ...);

#define LOG_DEBUG(fmt, ...) log_msg(LOG_LVL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_msg(LOG_LVL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_msg(LOG_LVL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_msg(LOG_LVL_ERROR, fmt, ##__VA_ARGS__)

int64_t get_time_us(void);
void format_bytes(uint64_t bytes, char *buf, size_t buf_len);
void format_time(int64_t seconds, char *buf, size_t buf_len);
void setup_signal_handlers(void (*handler)(int));

#endif /* UTILS_H */
