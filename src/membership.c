#include "membership.h"
#include "route.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

size_t membership_find(const membership_book *book, const char *id)
{
    for (size_t i = 0; i < book->count; i++) if (!strcmp(book->members[i].peer.id, id)) return i;
    return SIZE_MAX;
}

int membership_format(const chain_member *member, char *text, size_t capacity)
{
    int n = snprintf(text, capacity, "%s %s %u %" PRIu64 " %" PRIu64 " %d",
                     member->peer.id, member->peer.address, member->peer.port,
                     member->incarnation, member->ticket, member->provisional);
    return n < 0 || (size_t)n >= capacity ? -1 : n;
}

int membership_parse(const char *text, chain_member *member)
{
    *member = (chain_member){0}; char incarnation[21], ticket[21], port[6], provisional[2], extra;
    if (sscanf(text, "%32s %15s %5s %20s %20s %1s %c", member->peer.id, member->peer.address,
               port, incarnation, ticket, provisional, &extra) != 6 ||
        !sale_identifier(member->peer.id) || (provisional[0] != '0' && provisional[0] != '1')) return -1;
    unsigned int number = 0;
    for (char *p = port; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        number = number * 10 + (unsigned int)(*p - '0');
    }
    if (!number || number > 65535) return -1;
    member->peer.port = number;
    member->provisional = provisional[0] - '0';
    struct in_addr address;
    if (inet_pton(AF_INET, member->peer.address, &address) != 1) return -1;
    char *fields[2] = { incarnation, ticket };
    uint64_t *values[2] = { &member->incarnation, &member->ticket };
    for (size_t i = 0; i < 2; i++) {
        for (char *p = fields[i]; *p; p++) if (*p < '0' || *p > '9') return -1;
        errno = 0; char *end; uintmax_t n = strtoumax(fields[i], &end, 10);
        if (errno || *end || !n || n > UINT64_MAX) return -1;
        *values[i] = (uint64_t)n;
    }
    return 0;
}

static int apply(membership_book *book, const chain_member *member, int append)
{
    size_t position = membership_find(book, member->peer.id);
    if (position != SIZE_MAX) {
        chain_member *old = &book->members[position];
        if (old->incarnation > member->incarnation ||
            (old->incarnation == member->incarnation && old->ticket > member->ticket) ||
            (old->incarnation == member->incarnation && old->ticket == member->ticket &&
             old->provisional <= member->provisional)) return 0;
    } else {
        if (book->count == PEERS_MAX) { errno = ENOSPC; return -1; }
        position = book->count;
    }
    for (size_t i = 0; i < book->count; i++) if (i != position &&
        !strcmp(book->members[i].peer.address, member->peer.address) && book->members[i].peer.port == member->peer.port) {
        errno = EINVAL; return -1;
    }
    if (append) {
        char line[256]; int n = membership_format(member, line, sizeof line);
        if (n < 0) return -1;
        uint32_t crc = sale_crc(line, (size_t)n);
        int tail = snprintf(line + n, sizeof line - (size_t)n, "|%08" PRIx32 "\n", crc);
        if (tail < 0 || (size_t)tail >= sizeof line - (size_t)n || control_write(book->fd, line, (size_t)(n + tail)) < 0)
            return -1;
    }
    book->members[position] = *member;
    if (position == book->count) book->count++;
    if (member->ticket > book->clock) book->clock = member->ticket;
    book->revision++;
    return 1;
}

int membership_update(membership_book *book, const chain_member *member) { return apply(book, member, 1); }

int membership_finalize(membership_book *book)
{
    chain_member own = book->members[book->own];
    if (!own.provisional) return 0;
    if (book->clock == UINT64_MAX) { errno = EOVERFLOW; return -1; }
    own.ticket = book->clock + 1; own.provisional = 0;
    return membership_update(book, &own);
}

int membership_open(membership_book *book, const store_config *config)
{
    *book = (membership_book){ .fd = -1 };
    peer_catalog catalog;
    if (peers_load(config, &catalog) < 0) return -1;
    book->fd = control_open(config->data_dir, "membership.log"); if (book->fd < 0) return -1;
    int duplicate = dup(book->fd); if (duplicate < 0) goto failure;
    FILE *file = fdopen(duplicate, "r"); if (!file) { close(duplicate); goto failure; }
    char line[256]; off_t valid = 0; int failed = 0;
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n')) {
            if (!feof(file) || ftruncate(book->fd, valid) < 0) failed = 1;
            break;
        }
        size_t length = strlen(line); line[length - 1] = '\0';
        char *last = strrchr(line, '|'); char expected[9];
        if (!last || strlen(last + 1) != 8) { failed = 1; break; }
        snprintf(expected, sizeof expected, "%08" PRIx32, sale_crc(line, (size_t)(last - line)));
        if (strcmp(expected, last + 1)) { failed = 1; break; }
        *last = '\0'; chain_member member;
        if (membership_parse(line, &member) < 0 || apply(book, &member, 0) < 0) { failed = 1; break; }
        valid += (off_t)length;
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed || control_sync(book->fd) < 0 || lseek(book->fd, 0, SEEK_END) < 0) goto failure;
    for (size_t i = 0; i < catalog.count; i++) {
        size_t position = membership_find(book, catalog.entries[i].id);
        if (position == SIZE_MAX) {
            if (book->count == PEERS_MAX) goto failure;
            position = book->count++;
        }
        book->members[position].peer = catalog.entries[i];
    }
    book->own = membership_find(book, config->store_id);
    chain_member own = book->members[book->own];
    if (own.incarnation == UINT64_MAX || book->clock == UINT64_MAX) goto failure;
    own.incarnation++; own.ticket = book->clock + 1; own.provisional = 1;
    if (membership_update(book, &own) < 0) goto failure;
    return 0;
failure:
    membership_close(book); errno = EINVAL; return -1;
}

void membership_close(membership_book *book)
{
    if (book->fd >= 0) close(book->fd);
    book->fd = -1;
}
