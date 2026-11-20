#ifndef BLACKOUT_CONFIG_H
#define BLACKOUT_CONFIG_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    char store_id[33];
    char chain_id[33];
    char bind_address[16];
    unsigned int port;
    int64_t limit_cent;
    unsigned int timeout_ms;
    unsigned int queue_capacity;
    char data_dir[256];
    char peers_file[256];
} store_config;

/* No abre sockets ni crea archivos; valida una configuración completa. */
int config_load(const char *path, store_config *config, FILE *errors);

#endif
