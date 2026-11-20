#ifndef BLACKOUT_MEMBERSHIP_H
#define BLACKOUT_MEMBERSHIP_H
#include "peers.h"

typedef struct {
    peer_config peer;
    uint64_t incarnation, ticket;
    int provisional;
} chain_member;
typedef struct {
    int fd;
    chain_member members[PEERS_MAX];
    size_t count, own;
    uint64_t clock, revision;
} membership_book;
int membership_open(membership_book *book, const store_config *config);
size_t membership_find(const membership_book *book, const char *id);
/* 1 cambio; 0 igual/antiguo; -1 error de datos o disco. */
int membership_update(membership_book *book, const chain_member *member);
int membership_finalize(membership_book *book);
int membership_parse(const char *text, chain_member *member);
int membership_format(const chain_member *member, char *text, size_t capacity);
void membership_close(membership_book *book);
#endif
