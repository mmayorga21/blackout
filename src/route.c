#include "route.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int control_sync(int fd)
{
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    return result;
}

int control_write(int fd, const char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, data + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return control_sync(fd);
}

int control_open(const char *directory, const char *name)
{
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return -1;
    int fd = openat(dir, name, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) < 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 || control_sync(dir) < 0) {
        if (fd >= 0) close(fd);
        close(dir); errno = EINVAL; return -1;
    }
    if (close(dir) < 0) { close(fd); return -1; }
    return fd;
}

static int integer(const char *text, uint64_t *value)
{
    if (!*text) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end;
    uintmax_t result = strtoumax(text, &end, 10);
    if (errno || *end || result > UINT64_MAX) return -1;
    *value = (uint64_t)result; return 0;
}

int route_encode(const sale_route *route, char *wire, size_t capacity)
{
    int n = snprintf(wire, capacity, "R7|%" PRIu64 "|%zu|", route->version, route->count);
    if (n < 0 || (size_t)n >= capacity) return -1;
    size_t used = (size_t)n;
    for (size_t i = 0; i < route->count; i++) {
        n = snprintf(wire + used, capacity - used, "%s%s,%" PRIu64 ",%c", i ? ";" : "",
                     route->targets[i].id, route->targets[i].incarnation, route->targets[i].state);
        if (n < 0 || (size_t)n >= capacity - used) return -1;
        used += (size_t)n;
    }
    char csv[SALE_CSV_SIZE];
    n = sale_encode(&route->sale, csv, sizeof csv);
    if (n <= 0) return -1;
    csv[n - 1] = '\0';
    n = snprintf(wire + used, capacity - used, "|%s", csv);
    if (n < 0 || (size_t)n >= capacity - used) return -1;
    used += (size_t)n;
    uint32_t crc = sale_crc(wire, used);
    n = snprintf(wire + used, capacity - used, "|%08" PRIx32 "\n", crc);
    if (n < 0 || (size_t)n >= capacity - used) return -1;
    return (int)(used + (size_t)n);
}

int route_decode(const char *wire, sale_route *route)
{
    size_t length = strlen(wire);
    if (!length || length >= ROUTE_WIRE_SIZE || wire[length - 1] != '\n') return -1;
    char copy[ROUTE_WIRE_SIZE]; memcpy(copy, wire, length + 1); copy[length - 1] = '\0';
    char *last = strrchr(copy, '|');
    if (!last || strlen(last + 1) != 8) return -1;
    char expected[9];
    snprintf(expected, sizeof expected, "%08" PRIx32, sale_crc(copy, (size_t)(last - copy)));
    if (strcmp(expected, last + 1)) return -1;
    *last = '\0';
    char *parts[5], *cursor = copy;
    for (size_t i = 0; i < 4; i++) {
        parts[i] = cursor;
        cursor = strchr(cursor, '|'); if (!cursor) return -1;
        *cursor++ = '\0';
    }
    parts[4] = cursor;
    *route = (sale_route){0}; uint64_t count;
    if (strcmp(parts[0], "R7") || integer(parts[1], &route->version) < 0 || !route->version ||
        integer(parts[2], &count) < 0 || count >= PEERS_MAX) return -1;
    route->count = (size_t)count;
    char csv[SALE_CSV_SIZE];
    int n = snprintf(csv, sizeof csv, "%s\n", parts[4]);
    if (n < 0 || (size_t)n >= sizeof csv || sale_decode(csv, &route->sale) < 0) return -1;
    cursor = parts[3]; int pending = 0;
    for (size_t i = 0; i < route->count; i++) {
        char *next = strchr(cursor, ';');
        if (next) *next++ = '\0';
        char *inc = strchr(cursor, ','); if (!inc) return -1; *inc++ = '\0';
        char *state = strchr(inc, ','); if (!state) return -1; *state++ = '\0';
        if (!sale_identifier(cursor) || !strcmp(cursor, route->sale.origin) || integer(inc, &route->targets[i].incarnation) < 0 ||
            !route->targets[i].incarnation || strlen(state) != 1 || !strchr("PAS", *state)) return -1;
        if (*state == 'P') pending = 1;
        else if (pending) return -1;
        strcpy(route->targets[i].id, cursor); route->targets[i].state = *state;
        for (size_t j = 0; j < i; j++) if (!strcmp(route->targets[j].id, cursor)) return -1;
        if (i + 1 < route->count && !next) return -1;
        if (i + 1 == route->count && next) return -1;
        cursor = next;
    }
    if (!route->count && *parts[3]) return -1;
    return 0;
}

int route_same(const sale_route *a, const sale_route *b)
{
    if (strcmp(a->sale.id, b->sale.id) || strcmp(a->sale.stamp, b->sale.stamp) ||
        a->sale.amount != b->sale.amount || a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; i++)
        if (strcmp(a->targets[i].id, b->targets[i].id) || a->targets[i].incarnation != b->targets[i].incarnation) return 0;
    return 1;
}

size_t route_first_pending(const sale_route *route)
{
    for (size_t i = 0; i < route->count; i++) if (route->targets[i].state == 'P') return i;
    return route->count;
}

int route_eligible(const sale_route *route, const char *identity, uint64_t incarnation, int normal)
{
    for (size_t i = 0; i < route->count; i++) if (!strcmp(route->targets[i].id, identity)) {
        if (route->targets[i].incarnation != incarnation) return !normal;
        if (normal) return i == route_first_pending(route);
        return route->targets[i].state != 'P';
    }
    return !normal;
}

size_t route_log_find(const route_log *log, const char *id)
{
    if (!log->index_capacity) return SIZE_MAX;
    size_t slot = sale_crc(id, strlen(id)) & (log->index_capacity - 1);
    while (log->index[slot]) {
        size_t position = log->index[slot] - 1;
        if (!strcmp(log->records[position].sale.id, id)) return position;
        slot = (slot + 1) & (log->index_capacity - 1);
    }
    return SIZE_MAX;
}

static int reserve(route_log *log)
{
    if (log->count == log->capacity) {
        size_t capacity = log->capacity ? log->capacity * 2 : 32;
        if (capacity < log->capacity || capacity > SIZE_MAX / sizeof *log->records) { errno = ENOMEM; return -1; }
        sale_route *records = realloc(log->records, capacity * sizeof *records);
        if (!records) return -1;
        log->records = records; log->capacity = capacity;
    }
    if (log->count + 1 >= log->index_capacity / 2) {
        size_t capacity = log->index_capacity ? log->index_capacity * 2 : 64;
        if (capacity < log->index_capacity || capacity > SIZE_MAX / sizeof(size_t)) { errno = ENOMEM; return -1; }
        size_t *index = calloc(capacity, sizeof *index); if (!index) return -1;
        for (size_t i = 0; i < log->count; i++) {
            size_t slot = sale_crc(log->records[i].sale.id, strlen(log->records[i].sale.id)) & (capacity - 1);
            while (index[slot]) slot = (slot + 1) & (capacity - 1);
            index[slot] = i + 1;
        }
        free(log->index); log->index = index; log->index_capacity = capacity;
    }
    return 0;
}

static int insert(route_log *log, const sale_route *route, int append)
{
    size_t position = route_log_find(log, route->sale.id);
    if (position != SIZE_MAX) {
        const sale_route *old = &log->records[position];
        if (!route_same(old, route)) return 2;
        if (old->version > route->version) return 1;
        if (old->version == route->version) {
            for (size_t i = 0; i < route->count; i++) if (old->targets[i].state != route->targets[i].state) return 2;
            return 0;
        }
        for (size_t i = 0; i < route->count; i++)
            if (old->targets[i].state != 'P' && old->targets[i].state != route->targets[i].state) return 2;
    } else if (reserve(log) < 0) return -1;
    if (append) {
        char wire[ROUTE_WIRE_SIZE]; int n = route_encode(route, wire, sizeof wire);
        if (n < 0 || control_write(log->fd, wire, (size_t)n) < 0) return -1;
    }
    if (position == SIZE_MAX) {
        position = log->count++;
        size_t slot = sale_crc(route->sale.id, strlen(route->sale.id)) & (log->index_capacity - 1);
        while (log->index[slot]) slot = (slot + 1) & (log->index_capacity - 1);
        log->index[slot] = position + 1;
    }
    log->records[position] = *route;
    return 0;
}

int route_log_open(route_log *log, const char *directory)
{
    *log = (route_log){ .fd = -1 };
    log->fd = control_open(directory, "routes.log"); if (log->fd < 0) return -1;
    int copy = dup(log->fd); if (copy < 0) goto failure;
    FILE *file = fdopen(copy, "r"); if (!file) { close(copy); goto failure; }
    char line[ROUTE_WIRE_SIZE]; off_t valid = 0; int failed = 0;
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n')) {
            if (!feof(file) || ftruncate(log->fd, valid) < 0) failed = 1;
            break;
        }
        sale_route route;
        if (route_decode(line, &route) < 0 || insert(log, &route, 0) != 0) { failed = 1; break; }
        valid += (off_t)strlen(line);
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed || control_sync(log->fd) < 0 || lseek(log->fd, 0, SEEK_END) < 0) goto failure;
    return 0;
failure:
    route_log_close(log); errno = EINVAL; return -1;
}

int route_log_put(route_log *log, const sale_route *route) { return insert(log, route, 1); }
void route_log_close(route_log *log)
{
    if (log->fd >= 0) close(log->fd);
    free(log->records); free(log->index); *log = (route_log){ .fd = -1 };
}
