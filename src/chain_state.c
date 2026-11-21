#include "chain_state.h"
#include "route.h"
#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int chain_state_save(chain_state *state, int offline, int frozen)
{
    if (state->version && state->offline == offline && state->frozen == frozen) return 0;
    if (state->version == UINT64_MAX) { errno = EOVERFLOW; return -1; }
    char line[96];
    int n = snprintf(line, sizeof line, "S8|%" PRIu64 "|%d|%d", state->version + 1, offline, frozen);
    int tail = snprintf(line + n, sizeof line - (size_t)n, "|%08" PRIx32 "\n", sale_crc(line, (size_t)n));
    if (control_write(state->fd, line, (size_t)(n + tail)) < 0) return -1;
    state->version++; state->offline = offline; state->frozen = frozen;
    return 0;
}
int chain_state_open(chain_state *state, const char *directory, int fresh)
{
    *state = (chain_state){ .fd = control_open(directory, "chain.state") };
    if (state->fd < 0) return -1;
    int copy = dup(state->fd); if (copy < 0) goto fail;
    FILE *file = fdopen(copy, "r"); if (!file) { close(copy); goto fail; }
    char line[96]; off_t valid = 0; int failed = 0;
    while (fgets(line, sizeof line, file)) {
        size_t size = strlen(line);
        if (!strchr(line, '\n')) {
            if (!feof(file) || ftruncate(state->fd, valid) < 0) failed = 1;
            break;
        }
        char *crc = strrchr(line, '|'); char expected[10];
        if (!crc) { failed = 1; break; }
        snprintf(expected, sizeof expected, "%08" PRIx32 "\n", sale_crc(line, (size_t)(crc - line)));
        if (strcmp(expected, crc + 1)) { failed = 1; break; }
        *crc = '\0';
        char version[21], offline[2], frozen[2], extra;
        if (sscanf(line, "S8|%20[0-9]|%1[01]|%1[01]%c", version, offline, frozen, &extra) != 3) { failed = 1; break; }
        errno = 0; char *end; uintmax_t number = strtoumax(version, &end, 10);
        if (errno || *end || number != state->version + 1 || state->version == UINT64_MAX) { failed = 1; break; }
        state->version = (uint64_t)number; state->offline = offline[0] - '0'; state->frozen = frozen[0] - '0';
        valid += (off_t)size;
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed || control_sync(state->fd) < 0 || lseek(state->fd, 0, SEEK_END) < 0) goto fail;
    if (fresh && chain_state_save(state, 0, 0) < 0) goto fail;
    return 0;
fail:
    chain_state_close(state); errno = EINVAL; return -1;
}
void chain_state_close(chain_state *state)
{
    if (state->fd >= 0) close(state->fd);
    state->fd = -1;
}
