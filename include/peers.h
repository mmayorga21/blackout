#ifndef BLACKOUT_PEERS_H
#define BLACKOUT_PEERS_H

#include "config.h"

typedef struct {
    char id[33];
    char address[16];
    unsigned int port;
} peer_config;
enum { PEERS_MAX = 32 };
typedef struct { peer_config entries[PEERS_MAX]; size_t count, own; } peer_catalog;
/* El catálogo inicial contiene a la propia tienda y hasta 31 pares. */
int peers_load(const store_config *config, peer_catalog *catalog);

#endif
