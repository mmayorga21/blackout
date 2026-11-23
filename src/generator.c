#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "generator.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int natural(const char *text, uint64_t maximum, uint64_t *value)
{
    if (!*text) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end;
    uintmax_t n = strtoumax(text, &end, 10);
    if (errno || *end || n > maximum) return -1;
    *value = (uint64_t)n; return 0;
}

int generator_parse(int argc, char **argv, generator_options *options)
{
    if (argc != 4 && argc != 5) return -1;
    uint64_t minimum, maximum, interval, count, seed = 0;
    if (natural(argv[0], INT64_MAX, &minimum) < 0 || !minimum ||
        natural(argv[1], INT64_MAX, &maximum) < 0 || maximum < minimum ||
        natural(argv[2], 3600000, &interval) < 0 || !interval ||
        natural(argv[3], UINT64_MAX, &count) < 0 || !count ||
        (argc == 5 && natural(argv[4], UINT64_MAX, &seed) < 0)) return -1;
    *options = (generator_options){ .minimum = (int64_t)minimum, .maximum = (int64_t)maximum,
        .interval_ms = (unsigned int)interval, .count = count, .seed = seed, .seeded = argc == 5 };
    return 0;
}

/* SplitMix64: estado local al proceso, aritmética unsigned y secuencia repetible. */
static uint64_t next_random(uint64_t *state)
{
    uint64_t z = (*state += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static int random_seed(uint64_t *seed)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < sizeof *seed) {
        ssize_t n = read(fd, (char *)seed + used, sizeof *seed - used);
        if (n > 0) used += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else { int saved = n ? errno : EIO; close(fd); errno = saved; return -1; }
    }
    return close(fd);
}

int generator_run(const generator_options *options, pid_t parent)
{
    if (parent > 0) {
        /* No dejar un generador huérfano dormido si la tienda sufre SIGKILL. */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) return -1;
    }
    uint64_t state = options->seed;
    if (!options->seeded && random_seed(&state) < 0) { perror("Semilla del generador"); return -1; }
    uint64_t range = (uint64_t)(options->maximum - options->minimum) + 1;
    uint64_t threshold = (UINT64_C(0) - range) % range;
    for (uint64_t i = 0; i < options->count; i++) {
        uint64_t value;
        do { value = next_random(&state); } while (value < threshold);
        int64_t amount = options->minimum + (int64_t)(value % range);
        if (printf("venta %" PRId64 "\n", amount) < 0 || fflush(stdout) == EOF) return -1;
        if (i + 1 < options->count) {
            struct timespec delay = { .tv_sec = (time_t)(options->interval_ms / 1000),
                .tv_nsec = (long)(options->interval_ms % 1000) * 1000000 };
            int result;
            do { result = nanosleep(&delay, &delay); } while (result < 0 && errno == EINTR);
            if (result < 0) return -1;
        }
    }
    return 0;
}

int generator_start(purchase_generator *generator, const generator_options *options)
{
    if (generator->pid > 0) { errno = EBUSY; return -1; }
    int pipes[2];
    if (pipe(pipes) < 0) return -1;
    for (size_t i = 0; i < 2; i++) if (pipes[i] < 3) {
        int fd = fcntl(pipes[i], F_DUPFD_CLOEXEC, 3);
        if (fd < 0) goto fail;
        close(pipes[i]); pipes[i] = fd;
    }
    if (fcntl(pipes[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(pipes[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(pipes[0], F_SETFL, O_NONBLOCK) < 0) goto fail;
    char parent[32], minimum[32], maximum[32], interval[32], count[32], seed[32];
    snprintf(parent, sizeof parent, "%jd", (intmax_t)getpid());
    snprintf(minimum, sizeof minimum, "%" PRId64, options->minimum);
    snprintf(maximum, sizeof maximum, "%" PRId64, options->maximum);
    snprintf(interval, sizeof interval, "%u", options->interval_ms);
    snprintf(count, sizeof count, "%" PRIu64, options->count);
    snprintf(seed, sizeof seed, "%" PRIu64, options->seed);
    char *arguments[] = { "blackout", "--generador-tienda", parent, minimum, maximum, interval, count,
        options->seeded ? seed : NULL, NULL };
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (result) { errno = result; goto fail; }
    result = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!result) result = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    /* Cierra antes de exec incluso descriptores creados por otros hilos entre
     * socket/accept y FD_CLOEXEC. El hijo solo hereda los tres canales estándar. */
    if (!result) result = posix_spawn_file_actions_addclosefrom_np(&actions, 3);
    pid_t pid;
    if (!result) result = posix_spawn(&pid, "/proc/self/exe", &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result) { errno = result; goto fail; }
    close(pipes[1]);
    *generator = (purchase_generator){ .pid = pid, .fd = pipes[0] };
    return 0;
fail: {
    int saved = errno; close(pipes[0]); close(pipes[1]); errno = saved; return -1;
}
}

int generator_read(purchase_generator *generator, int64_t *amount)
{
    char c;
    ssize_t n = read(generator->fd, &c, 1);
    while (n == 1) {
        if (c == '\n') {
            generator->line[generator->used] = '\0';
            uint64_t value;
            if (strncmp(generator->line, "venta ", 6) || natural(generator->line + 6, INT64_MAX, &value) < 0 || !value) {
                errno = EPROTO; return -1;
            }
            *amount = (int64_t)value; generator->used = 0; return 1;
        }
        if (!c || generator->used == sizeof generator->line - 1) { errno = EPROTO; return -1; }
        generator->line[generator->used++] = c;
        n = read(generator->fd, &c, 1);
    }
    if (n == 0) { if (generator->used) { errno = EPROTO; return -1; } return 2; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

int generator_finish(purchase_generator *generator, int terminate)
{
    int saved = 0, status = 0;
    if (generator->pid > 0) {
        if (terminate && kill(generator->pid, SIGTERM) < 0 && errno != ESRCH) saved = errno;
        if (generator->fd >= 0 && close(generator->fd) < 0 && !saved) saved = errno;
        pid_t result;
        do { result = waitpid(generator->pid, &status, 0); } while (result < 0 && errno == EINTR);
        if (result < 0 && !saved) saved = errno;
        if (!terminate && result > 0 && (!WIFEXITED(status) || WEXITSTATUS(status)) && !saved) saved = EIO;
    }
    *generator = (purchase_generator){ .fd = -1 };
    if (saved) { errno = saved; return -1; }
    return 0;
}
