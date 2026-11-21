#ifndef BLACKOUT_STORE_H
#define BLACKOUT_STORE_H

#include "journal.h"

typedef struct {
    store_config config;
    sales_journal journal;
    int storage_failed, closing;
} local_store;

int store_open(local_store *store, const store_config *config);
/* 0: aceptada; 1: rechazada sin modificar disco; -1: fallo de almacenamiento. */
int store_can_sell(local_store *store, int64_t amount);
int store_sell(local_store *store, int64_t amount);
void store_status(const local_store *store);
int store_at_limit(const local_store *store);
int store_close(local_store *store);

#endif
