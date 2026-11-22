#ifndef BLACKOUT_QUERY_H
#define BLACKOUT_QUERY_H
#include "config.h"
#include "work_queue.h"
#include "report.h"
typedef struct query_runtime query_runtime;
query_runtime *query_start(const store_config *config, work_queue *queue);
void query_notify(query_runtime *query);
void query_reply(query_runtime *query, const work_request *request, const store_report *report);
void query_stop(query_runtime *query);
void query_destroy(query_runtime *query);
#endif
