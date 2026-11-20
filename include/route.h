#ifndef BLACKOUT_ROUTE_H
#define BLACKOUT_ROUTE_H

#include "peers.h"
#include "sale.h"

enum { ROUTE_WIRE_SIZE = 4096 };
typedef struct { char id[33]; uint64_t incarnation; char state; } route_target;
typedef struct {
    sale_record sale;
    uint64_t version;
    size_t count;
    route_target targets[PEERS_MAX];
} sale_route;
typedef struct {
    int fd;
    sale_route *records;
    size_t count, capacity, *index, index_capacity;
} route_log;

int control_write(int fd, const char *data, size_t length);
int control_open(const char *directory, const char *name);
int control_sync(int fd);
int route_encode(const sale_route *route, char *wire, size_t capacity);
int route_decode(const char *wire, sale_route *route);
int route_same(const sale_route *a, const sale_route *b);
/* 1: puede aplicar; 0: turno aún no autorizado. */
int route_eligible(const sale_route *route, const char *identity, uint64_t incarnation, int normal);
size_t route_first_pending(const sale_route *route);
int route_log_open(route_log *log, const char *directory);
size_t route_log_find(const route_log *log, const char *id);
/* 0 guardado/igual; 1 antiguo; 2 conflicto; -1 error de almacenamiento. */
int route_log_put(route_log *log, const sale_route *route);
void route_log_close(route_log *log);

#endif
