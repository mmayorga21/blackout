#ifndef BLACKOUT_NETWORK_H
#define BLACKOUT_NETWORK_H

#include "journal.h"
#include "work_queue.h"

typedef struct network_runtime network_runtime;
network_runtime *network_start(const store_config *config, const sales_journal *journal,
                               work_queue *queue, int console_wake);
int network_publish(network_runtime *network, const sale_record *sale);
void network_reply(network_runtime *network, uint64_t connection, const char *id, int accepted);
void network_notify(network_runtime *network);
void network_status(network_runtime *network);
/* Detener/join antes de cerrar productores; liberar solo tras join del trabajador. */
void network_quiesce(network_runtime *network);
void network_destroy(network_runtime *network);

#endif
