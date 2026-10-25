#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include "log.h"

static void print_timestamp(void) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    printf("[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
}

void cyber_log(const char *fmt, ...) {
    print_timestamp();
    printf("[Cyber] ");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

void cyber_warn(const char *fmt, ...) {
    print_timestamp();
    printf("[Warn]  ");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

void cyber_error(const char *fmt, ...) {
    print_timestamp();
    printf("[Error] ");
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}
