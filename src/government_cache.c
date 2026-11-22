#include "government.h"
#include "route.h"
#include "display.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CACHE_LINE = REPORT_WIRE_SIZE + 256 };
static int directories(const char *path)
{
    char copy[256]; strcpy(copy, path); size_t length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/') copy[--length] = '\0';
    for (size_t i = 1; i <= length; i++) {
        if (i != length && copy[i] != '/') continue;
        char saved = copy[i]; copy[i] = '\0';
        int made = mkdir(copy, 0700) == 0;
        if (!made && errno != EEXIST) return -1;
        int dir = open(copy, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (dir < 0) return -1;
        int result = made ? control_sync(dir) : 0;
        if (close(dir) < 0) result = -1;
        if (result < 0) return -1;
        if (made) {
            char parent[256]; strcpy(parent, copy); char *slash = strrchr(parent, '/');
            if (!slash) strcpy(parent, "."); else if (slash == parent) slash[1] = '\0'; else *slash = '\0';
            dir = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC); if (dir < 0) return -1;
            result = control_sync(dir); if (close(dir) < 0) result = -1;
            if (result < 0) return -1;
        }
        copy[i] = saved;
    }
    return 0;
}
static int timestamp(const char *text)
{
    if (strlen(text) != 24) return 0;
    for (size_t i = 0; i < 24; i++) {
        if (i == 4 || i == 7) { if (text[i] != '-') return 0; }
        else if (i == 10) { if (text[i] != 'T') return 0; }
        else if (i == 13 || i == 16) { if (text[i] != ':') return 0; }
        else if (i == 19) { if (text[i] != '+' && text[i] != '-') return 0; }
        else if (text[i] < '0' || text[i] > '9') return 0;
    }
    return 1;
}
static int load_row(char *line, government_cache *cache, const government_config *config)
{
    size_t length = strlen(line); if (!length || line[length - 1] != '\n') return -1;
    line[length - 1] = '\0';
    char *last = strrchr(line, '|'), expected[9];
    if (!last || strlen(last + 1) != 8) return -1;
    snprintf(expected, sizeof expected, "%08" PRIx32, sale_crc(line, (size_t)(last - line)));
    if (strcmp(expected, last + 1)) return -1;
    *last = '\0';
    char *parts[8], *cursor = line;
    for (size_t i = 0; i < 7; i++) {
        parts[i] = cursor; cursor = strchr(cursor, '|'); if (!cursor) return -1; *cursor++ = '\0';
    }
    parts[7] = cursor;
    if (strcmp(parts[0], "G9") || !sale_identifier(parts[1]) || !sale_identifier(parts[2]) || !timestamp(parts[5])) return -1;
    struct in_addr address;
    if (inet_pton(AF_INET, parts[3], &address) != 1 || !*parts[4]) return -1;
    for (const char *p = parts[4]; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end; unsigned long port = strtoul(parts[4], &end, 10);
    if (errno || *end || !port || port > 65535) return -1;
    if (!*parts[6]) return -1;
    for (const char *p = parts[6]; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; uintmax_t observed = strtoumax(parts[6], &end, 10);
    if (errno || *end || observed > INT64_MAX) return -1;
    char wire[REPORT_WIRE_SIZE]; int n = snprintf(wire, sizeof wire, "%s\n", parts[7]); store_report report;
    if (n < 0 || (size_t)n >= sizeof wire || report_decode(wire, &report) < 0 ||
        strcmp(parts[1], report.store_id) || strcmp(parts[2], report.chain_id)) return -1;
    for (size_t i = 0; i < config->count; i++) {
        const government_endpoint *endpoint = &config->stores[i];
        if (!strcmp(endpoint->id, parts[1]) && !strcmp(endpoint->chain, parts[2]) &&
            !strcmp(endpoint->address, parts[3]) && endpoint->port == port) {
            cache->latest[i] = (government_observation){ .present = 1, .observed_at = (int64_t)observed, .report = report };
            strcpy(cache->latest[i].received, parts[5]); break;
        }
    }
    return 0;
}
int government_cache_open(government_cache *cache, const government_config *config)
{
    *cache = (government_cache){ .fd = -1, .lock_fd = -1 };
    if (directories(config->data_dir) < 0) goto fail;
    cache->lock_fd = control_open(config->data_dir, "government.lock"); if (cache->lock_fd < 0) goto fail;
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(cache->lock_fd, F_SETLK, &lock) < 0) goto fail;
    cache->fd = control_open(config->data_dir, "consultas.log"); if (cache->fd < 0) goto fail;
    int copy = dup(cache->fd); if (copy < 0) goto fail;
    FILE *file = fdopen(copy, "r"); if (!file) { close(copy); goto fail; }
    char line[CACHE_LINE]; off_t valid = 0; int failed = 0;
    while (fgets(line, sizeof line, file)) {
        size_t length = strlen(line);
        if (!strchr(line, '\n')) {
            if (!feof(file) || ftruncate(cache->fd, valid) < 0) failed = 1;
            else fputs("Recuperación de consultas: última fila incompleta descartada.\n", display_errors());
            break;
        }
        if (load_row(line, cache, config) < 0) { failed = 1; break; }
        valid += (off_t)length;
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed) { errno = EINVAL; goto fail; }
    if (control_sync(cache->fd) < 0 || lseek(cache->fd, 0, SEEK_END) < 0) goto fail;
    return 0;
fail:
    display_perror("Abrir historial de consultas (se conserva para diagnóstico)");
    government_cache_close(cache); return -1;
}
int government_cache_save(government_cache *cache, const government_config *config, size_t index,
                          const government_observation *observation)
{
    const store_report *report = &observation->report;
    const char *received = observation->received;
    const government_endpoint *endpoint = &config->stores[index];
    char wire[REPORT_WIRE_SIZE]; int n = report_encode(report, wire, sizeof wire); if (n < 0) return -1;
    wire[n - 1] = '\0';
    char line[CACHE_LINE];
    n = snprintf(line, sizeof line, "G9|%s|%s|%s|%u|%s|%" PRId64 "|%s", endpoint->id, endpoint->chain, endpoint->address, endpoint->port, received, observation->observed_at, wire);
    if (n < 0 || (size_t)n >= sizeof line) return -1;
    int tail = snprintf(line + n, sizeof line - (size_t)n, "|%08" PRIx32 "\n", sale_crc(line, (size_t)n));
    if (tail < 0 || (size_t)tail >= sizeof line - (size_t)n || control_write(cache->fd, line, (size_t)(n + tail)) < 0) return -1;
    cache->latest[index] = *observation; return 0;
}
void government_cache_close(government_cache *cache)
{
    if (cache->fd >= 0) close(cache->fd);
    if (cache->lock_fd >= 0) close(cache->lock_fd);
    cache->fd = cache->lock_fd = -1;
}
