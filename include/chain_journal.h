#ifndef BLACKOUT_CHAIN_JOURNAL_H
#define BLACKOUT_CHAIN_JOURNAL_H

#include "journal.h"
#include "sale.h"

int chain_open(sales_journal *journal, const store_config *config);
int chain_local(sales_journal *journal, int64_t amount);
/* 0 nueva; 1 duplicada; 2 inválida/conflicto; -1 fallo de almacenamiento. */
int chain_receive(sales_journal *journal, const sale_record *sale);
int chain_record(const sales_journal *journal, size_t index, sale_record *sale);
int chain_history(const sales_journal *journal);
int chain_close(sales_journal *journal);

#endif
