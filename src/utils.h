#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stddef.h>
#include "logger.h"

int64_t get_time_us(void);
void format_bytes(uint64_t bytes, char *buf, size_t buf_len);
void format_time(int64_t seconds, char *buf, size_t buf_len);
void setup_signal_handlers(void (*handler)(int));

#endif /* UTILS_H */
