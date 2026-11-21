#ifndef BLACKOUT_NETWORK_H
#define BLACKOUT_NETWORK_H

#include "journal.h"
#include "work_queue.h"

typedef struct network_runtime network_runtime;
network_runtime *network_start(const store_config *config, sales_journal *journal,
                               work_queue *queue, int console_wake);
/* Solo las ventas propias recién registradas esperan antes de compartirse. */
int network_local(network_runtime *network, sales_journal *journal, int64_t amount);
int network_maintain(network_runtime *network, int64_t total);
int network_frozen(network_runtime *network);
int network_connection(network_runtime *network, int connected);
int network_close_sales(network_runtime *network);
int network_checkpoint(network_runtime *network);
void network_prepare_exit(network_runtime *network);
void network_wait_exit(network_runtime *network);
int network_publish(network_runtime *network, const sale_record *sale, int defer);
/* Resultados: 0 nueva, 1 duplicada, 2 conflicto, 3 turno diferido, -1 error. */
int network_receive(network_runtime *network, const work_request *request, sales_journal *journal);
int network_advance(network_runtime *network, const work_request *request);
void network_reply(network_runtime *network, const work_request *request, const char *id, int result);
void network_notify(network_runtime *network);
void network_status(network_runtime *network);
/* Detener/join antes de cerrar productores; liberar solo tras join del trabajador. */
void network_quiesce(network_runtime *network);
void network_destroy(network_runtime *network);

#endif
