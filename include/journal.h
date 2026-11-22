#ifndef BLACKOUT_JOURNAL_H
#define BLACKOUT_JOURNAL_H

#include "config.h"

/* Tras abrir: solo el trabajador muta contabilidad; consultas usan su misma cola. */
typedef struct {
    struct chain_data *chain;
    int fd;
    int metadata_fd;
    int legacy_fd;
    char store_id[33];
    uint64_t count;
    int64_t total_cent;
    int64_t own_total_cent;
    uint64_t own_sequence;
    int fresh_history;
} sales_journal;

int journal_open(sales_journal *journal, const store_config *config);
/* Un error de escritura o sincronización obliga a detener esta instancia. */
int journal_append(sales_journal *journal, int64_t amount);
int journal_history(const sales_journal *journal);
int journal_close(sales_journal *journal);

#endif
