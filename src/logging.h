#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <stdbool.h>

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

bool log_init(const char *filepath);
void log_close(void);
void log_set_level(LogLevel level);
LogLevel log_get_level(void);
void log_message(LogLevel level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_DEBUG(tag, ...) do { if (log_get_level() <= LOG_LEVEL_DEBUG) log_message(LOG_LEVEL_DEBUG, tag, __VA_ARGS__); } while(0)
#define LOG_INFO(tag, ...)  do { if (log_get_level() <= LOG_LEVEL_INFO)  log_message(LOG_LEVEL_INFO, tag, __VA_ARGS__); } while(0)
#define LOG_WARN(tag, ...)  log_message(LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) log_message(LOG_LEVEL_ERROR, tag, __VA_ARGS__)

const char *log_get_filepath(void);

#endif
