/* Interposición solo para pruebas: nunca se enlaza con el ejecutable. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int sale_started = 0;

static int is_journal(int fd)
{
    char link[64], path[4096];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t length = readlink(link, path, sizeof path - 1);
    if (length < 0) return 0;
    path[length] = '\0';
    size_t n = strlen(path), suffix = strlen("/sales.csv");
    return n >= suffix && strcmp(path + n - suffix, "/sales.csv") == 0;
}

ssize_t write(int fd, const void *buffer, size_t length)
{
    ssize_t (*real_write)(int, const void *, size_t);
    void *symbol = dlsym(RTLD_NEXT, "write");
    memcpy(&real_write, &symbol, sizeof real_write);
    if (real_write == NULL) _exit(98);
    const char *mode = getenv("BLACKOUT_TEST_FAULT");
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
    if (mode && is_journal(fd)) {
        static int interrupted = 0;
        if (strcmp(mode, "eintr") == 0 && !interrupted) {
            interrupted = 1; errno = EINTR; return -1;
        }
        if (strcmp(mode, "sync_error") == 0 && sale_started) { errno = EIO; return -1; }
    }
    return real_sync(fd);
}
