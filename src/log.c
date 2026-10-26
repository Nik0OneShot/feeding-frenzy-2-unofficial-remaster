#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "log.h"

static FILE *log_file;

void log_open(void)
{
    log_file = fopen("ff2wide.log", "w");
}

void log_printf(const char *fmt, ...)
{
    if (!log_file) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fputc('\n', log_file);
    fflush(log_file);
}
