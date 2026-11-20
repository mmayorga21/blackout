#ifndef BLACKOUT_PEERS_H
#define BLACKOUT_PEERS_H

#include "config.h"

typedef struct {
    char id[33];
    char address[16];
    unsigned int port;
} peer_config;

/* Etapa 6: catálogo completo de exactamente dos tiendas, incluida la propia. */
int peers_load(const store_config *config, peer_config *other);

#endif
