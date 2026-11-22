/* Interposición solo para pruebas: nunca se enlaza con el ejecutable. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int sale_started = 0;

static int route_started = 0;
static int cache_started = 0;

static int is_named(int fd, const char *suffix)
{
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t length = readlink(link, path, sizeof path - 1);
    if (length < 0) return 0;
    path[length] = '\0';
    size_t n = strlen(path), tail = strlen(suffix);
    return n >= tail && strcmp(path + n - tail, suffix) == 0;
}

static int is_journal(int fd) { return is_named(fd, "/sales.csv"); }

ssize_t write(int fd, const void *buffer, size_t length)
{
    ssize_t (*real_write)(int, const void *, size_t);
    void *symbol = dlsym(RTLD_NEXT, "write");
    memcpy(&real_write, &symbol, sizeof real_write);
    if (real_write == NULL) _exit(98);
    const char *mode = getenv("BLACKOUT_TEST_FAULT");
    if (mode && is_named(fd, "/consultas.log") && length >= 3 && !memcmp(buffer, "G9|", 3)) {
        cache_started = 1;
        if (!strcmp(mode, "cache_enospc")) { errno = ENOSPC; return -1; }
    }
    if (mode && is_named(fd, "/routes.log") && length >= 3 && !memcmp(buffer, "R7|", 3)) {
        route_started = 1;
        if (!strcmp(mode, "route_partial_exit")) {
            if (real_write(fd, buffer, 12) != 12) _exit(97);
            _exit(79);
        }
        if (!strcmp(mode, "route_full_exit")) {
            if (real_write(fd, buffer, length) != (ssize_t)length) _exit(97);
            _exit(80);
        }
        if (!strcmp(mode, "route_enospc")) { errno = ENOSPC; return -1; }
    }
    if (mode && is_journal(fd)) {
        if (length > 12 && memchr(buffer, ':', length) != NULL) {
            sale_started = 1;
            if (strcmp(mode, "partial_exit") == 0) {
                if (real_write(fd, buffer, 12) != 12) _exit(97);
                _exit(77);
            }
            if (strcmp(mode, "full_exit") == 0) {
                if (real_write(fd, buffer, length) != (ssize_t)length) _exit(97);
                _exit(78);
            }
            if (strcmp(mode, "enospc") == 0) { errno = ENOSPC; return -1; }
            if (strcmp(mode, "zero_write") == 0) return 0;
        }
        if (strcmp(mode, "short_write") == 0 && length > 3) length = 3;
        static int interrupted = 0;
        if (strcmp(mode, "eintr") == 0 && !interrupted) {
            interrupted = 1; errno = EINTR; return -1;
        }
    }
    return real_write(fd, buffer, length);
}

int fsync(int fd)
{
    int (*real_sync)(int);
    void *symbol = dlsym(RTLD_NEXT, "fsync");
    memcpy(&real_sync, &symbol, sizeof real_sync);
    if (real_sync == NULL) _exit(98);
    const char *mode = getenv("BLACKOUT_TEST_FAULT");
    if (mode && !strcmp(mode, "cache_sync_error") && cache_started && is_named(fd, "/consultas.log")) { errno = EIO; return -1; }
    if (mode && ((!strcmp(mode, "route_sync_error") && route_started && is_named(fd, "/routes.log")) ||
                 (!strcmp(mode, "certificate_sync_error") && is_named(fd, "/.close.pending")))) { errno = EIO; return -1; }
    if (mode && is_journal(fd)) {
        static int interrupted = 0;
        if (strcmp(mode, "eintr") == 0 && !interrupted) {
            interrupted = 1; errno = EINTR; return -1;
        }
        if (strcmp(mode, "sync_error") == 0 && sale_started) { errno = EIO; return -1; }
        if (strcmp(mode, "slow_sync") == 0 && sale_started) {
            struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000 };
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR) { }
        }
    }
    return real_sync(fd);
}
