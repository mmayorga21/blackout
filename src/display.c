#include "display.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local FILE *buffer;
static _Thread_local char *text;
static _Thread_local size_t length;
static _Thread_local char title[320];
static _Thread_local int error_output;

static void lock_output(int unlock)
{
    int result = unlock ? pthread_mutex_unlock(&output_mutex) : pthread_mutex_lock(&output_mutex);
    if (result) exit(EXIT_FAILURE);
}

void display_begin(const char *format, ...)
{
    if (buffer) exit(EXIT_FAILURE);
    va_list args;
    va_start(args, format);
    vsnprintf(title, sizeof title, format, args);
    va_end(args);
    text = NULL; length = 0; error_output = 0;
    buffer = open_memstream(&text, &length);
    if (!buffer) { perror("Preparar salida de consola"); exit(EXIT_FAILURE); }
}

FILE *display_output(void) { return buffer ? buffer : stdout; }
FILE *display_errors(void)
{
    if (!buffer) return stderr;
    error_output = 1;
    return buffer;
}

void display_end(void)
{
    if (!buffer) return;
    if (fclose(buffer) < 0) exit(EXIT_FAILURE);
    buffer = NULL;
    if (!length) { free(text); text = NULL; return; }
    FILE *destination = error_output ? stderr : stdout;
    lock_output(0);
    int notice = !strncmp(title, "AVISO DE RED", 12);
    fprintf(destination, notice ? "\n---------- %s ----------\n" : "\n========== %s ==========\n", title);
    if (length) fwrite(text, 1, length, destination);
    if (length && text[length - 1] != '\n') fputc('\n', destination);
    fputs(notice ? "----------------------------------------\n" : "========================================\n", destination);
    fflush(destination);
    lock_output(1);
    free(text); text = NULL;
}

void display_perror(const char *message)
{
    int saved = errno;
    int standalone = !buffer;
    if (standalone) display_begin("ERROR");
    fprintf(display_errors(), "%s: %s\n", message, strerror(saved));
    if (standalone) display_end();
    errno = saved;
}

void display_notice(const char *format, ...)
{
    va_list args;
    lock_output(0);
    fputs("\n---------- AVISO DE RED -----------------\n", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("----------------------------------------\n", stderr);
    fflush(stderr);
    lock_output(1);
}

void display_prompt(void)
{
    lock_output(0);
    fputs("blackout> ", stdout); fflush(stdout);
    lock_output(1);
}
