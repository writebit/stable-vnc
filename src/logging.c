#include "logging.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static FILE *g_log_file = NULL;
static LogLevel g_log_level = LOG_LEVEL_WARN;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char g_log_filepath[1024] = {0};

static const char *level_strings[] = {
    "DEBUG", "INFO", "WARN", "ERROR"
};

bool log_init(const char *filepath) {
    pthread_mutex_lock(&g_log_mutex);

    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }

    if (filepath) {
        strncpy(g_log_filepath, filepath, sizeof(g_log_filepath) - 1);
    } else {
        // Default: ~/Library/Logs/StableVNC.log
        const char *home = getenv("HOME");
        if (home) {
            snprintf(g_log_filepath, sizeof(g_log_filepath),
                     "%s/Library/Logs/StableVNC.log", home);
        } else {
            snprintf(g_log_filepath, sizeof(g_log_filepath), "/tmp/StableVNC.log");
        }
    }

    g_log_file = fopen(g_log_filepath, "a");
    if (!g_log_file) {
        fprintf(stderr, "Failed to open log file: %s\n", g_log_filepath);
        pthread_mutex_unlock(&g_log_mutex);
        return false;
    }

    pthread_mutex_unlock(&g_log_mutex);

    log_message(LOG_LEVEL_INFO, "Log", "=== StableVNC started, log file: %s ===", g_log_filepath);
    return true;
}

void log_close(void) {
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        fprintf(g_log_file, "\n");
        fclose(g_log_file);
        g_log_file = NULL;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void log_set_level(LogLevel level) {
    g_log_level = level;
}

LogLevel log_get_level(void) {
    return g_log_level;
}

void log_message(LogLevel level, const char *tag, const char *fmt, ...) {
    if (level < g_log_level) return;

    time_t now;
    time(&now);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    va_list args;

    // Always print to stderr
    va_start(args, fmt);
    fprintf(stderr, "%s [%s] [%s] ", time_str, level_strings[level], tag);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);

    // Also write to log file
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        va_start(args, fmt);
        fprintf(g_log_file, "%s [%s] [%s] ", time_str, level_strings[level], tag);
        vfprintf(g_log_file, fmt, args);
        fprintf(g_log_file, "\n");
        fflush(g_log_file);
        va_end(args);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

const char *log_get_filepath(void) {
    return g_log_filepath;
}
