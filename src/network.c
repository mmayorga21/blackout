#include "display.h"
#include "network.h"
#include "chain_journal.h"
#include "membership.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { HEADER = 12, MAX_CONNECTIONS = PEERS_MAX * 2, HELLO = 1, SALE = 2, ACK = 3,
       PING = 4, PONG = 5, REJECT = 6, MEMBERS = 7, NOTICE = 8, RECOVER = 9, DEFER = 10, BYE = 11 };
typedef struct {
    sale_route route;
    int applied, control_pending, restore_queued;
    int64_t ready_at, target_since;
    char event;
    size_t event_target;
    uint64_t event_version;
    uint64_t ack_token[PEERS_MAX], ack_version[PEERS_MAX];
} shared_route;
typedef struct { uint64_t token; int code; char id[SALE_ID_SIZE]; } reply_slot;
typedef struct { char id[96], text[160]; } chain_notice;
typedef struct {
    int fd, outgoing, connecting, ready, legacy, hello_sent, waiting_ack, waiting_worker, pending, pong, leaving;
    size_t peer, tx_length, tx_sent, rx_length, outstanding_target, notice_cursor;
    uint64_t token, sent_revision, outstanding_version, incarnation;
    int outstanding_normal;
    char outstanding[SALE_ID_SIZE];
    int64_t started, last_rx, last_tx, sent_at;
    unsigned char rx[ROUTE_WIRE_SIZE * 2], tx[HEADER + ROUTE_WIRE_SIZE];
    work_request incoming;
} peer_connection;

struct network_runtime {
    store_config config;
    work_queue *queue;
    int console_wake, wake[2], listener, stopping, joined;
    pthread_t thread;
    pthread_mutex_t mutex;
    membership_book book;             /* Solo el hilo de red la modifica. */
    route_log routes;                 /* Solo el trabajador la modifica. */
    chain_member view[PEERS_MAX];     /* Instantánea protegida para el trabajador. */
    int online[PEERS_MAX];
    uint64_t online_token[PEERS_MAX];
    size_t member_count, own;
    shared_route *records;
    size_t count, capacity;
    reply_slot replies[MAX_CONNECTIONS];
    peer_connection connections[MAX_CONNECTIONS];
    int active[PEERS_MAX];
    int lost_pending[PEERS_MAX];
    int64_t retry[PEERS_MAX];
    uint64_t next_token, notice_sequence;
    chain_notice *notices;
    size_t notice_count, notice_capacity;
};

static void checked(int result)
{
    if (result) { fprintf(display_errors(), "Sincronización de red: %s\n", strerror(result)); exit(EXIT_FAILURE); }
}
static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) { display_perror("Reloj de red"); exit(EXIT_FAILURE); }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? -1 : 0;
}
void network_notify(network_runtime *network)
{
    if (!network) return;
    ssize_t n;
    do { n = write(network->wake[1], "x", 1); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { display_perror("Notificar red"); exit(EXIT_FAILURE); }
}
static void drain(int fd)
{
    char data[256];
    for (;;) {
        ssize_t n = read(fd, data, sizeof data);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;
    }
}
static int stopping(network_runtime *network)
{
    checked(pthread_mutex_lock(&network->mutex)); int result = network->stopping;
    checked(pthread_mutex_unlock(&network->mutex)); return result;
}
static void fatal_network(network_runtime *network, const char *message)
{
    display_perror(message); queue_stop(network->queue, QUEUE_ERROR);
    ssize_t n;
    do { n = write(network->console_wake, "x", 1); } while (n < 0 && errno == EINTR);
}
static void refresh_view(network_runtime *network)
{
    checked(pthread_mutex_lock(&network->mutex));
    network->member_count = network->book.count; network->own = network->book.own;
    for (size_t i = 0; i < network->book.count; i++) {
        network->view[i] = network->book.members[i];
        int slot = network->active[i];
        network->online[i] = i == network->own || (slot >= 0 && network->connections[slot].ready &&
                             !network->connections[slot].leaving && !network->view[i].provisional &&
                             network->connections[slot].incarnation == network->view[i].incarnation);
        network->online_token[i] = slot >= 0 ? network->connections[slot].token : 0;
    }
    checked(pthread_mutex_unlock(&network->mutex));
}
static size_t record_find(network_runtime *network, const char *id)
{
    for (size_t i = 0; i < network->count; i++) if (!strcmp(network->records[i].route.sale.id, id)) return i;
    return SIZE_MAX;
}
static int merge_record(network_runtime *network, const sale_route *route, int applied, int64_t ready)
{
    checked(pthread_mutex_lock(&network->mutex));
    size_t position = record_find(network, route->sale.id);
    if (position == SIZE_MAX) {
        if (network->count == network->capacity) {
            size_t capacity = network->capacity ? network->capacity * 2 : 32;
            if (capacity < network->capacity || capacity > SIZE_MAX / sizeof *network->records) goto no_memory;
            shared_route *records = realloc(network->records, capacity * sizeof *records);
            if (!records) goto no_memory;
            network->records = records; network->capacity = capacity;
        }
        position = network->count++;
        network->records[position] = (shared_route){ .route = *route, .ready_at = ready };
    } else if (route->version >= network->records[position].route.version) network->records[position].route = *route;
    if (applied) network->records[position].applied = 1;
    checked(pthread_mutex_unlock(&network->mutex));
    network_notify(network); return 0;
no_memory:
    checked(pthread_mutex_unlock(&network->mutex)); errno = ENOMEM; return -1;
}

int network_publish(network_runtime *network, const sale_record *sale, int defer)
{
    if (!network) return 0;
    size_t found = route_log_find(&network->routes, sale->id);
    sale_route route = { .sale = *sale, .version = 1 };
    if (found != SIZE_MAX) route = network->routes.records[found];
    else if (defer) {
        chain_member sorted[PEERS_MAX]; size_t count = 0;
        checked(pthread_mutex_lock(&network->mutex));
        for (size_t i = 0; i < network->member_count; i++)
            if (i != network->own && network->online[i] && !network->view[i].provisional) sorted[count++] = network->view[i];
        checked(pthread_mutex_unlock(&network->mutex));
        for (size_t i = 1; i < count; i++) {
            chain_member value = sorted[i]; size_t j = i;
            while (j && (sorted[j - 1].ticket > value.ticket ||
                   (sorted[j - 1].ticket == value.ticket && strcmp(sorted[j - 1].peer.id, value.peer.id) > 0))) {
                sorted[j] = sorted[j - 1]; j--;
            }
            sorted[j] = value;
        }
        route.count = count;
        for (size_t i = 0; i < count; i++) {
            strcpy(route.targets[i].id, sorted[i].peer.id);
            route.targets[i].incarnation = sorted[i].incarnation; route.targets[i].state = 'P';
        }
    }
    if (found == SIZE_MAX && route_log_put(&network->routes, &route) != 0) return -1;
    return merge_record(network, &route, 1, defer ? milliseconds() + network->config.sale_share_delay_ms : 0);
}

int network_receive(network_runtime *network, const work_request *request, sales_journal *journal)
{
    sale_route incoming;
    if (route_decode(request->route_wire, &incoming) < 0) return 2;
    if (request->normal && strcmp(request->sender, incoming.sale.origin)) return 2;
    int saved = route_log_put(&network->routes, &incoming);
    if (saved < 0) return -1;
    if (saved == 2) return 2;
    size_t position = route_log_find(&network->routes, incoming.sale.id);
    sale_route route = network->routes.records[position];
    checked(pthread_mutex_lock(&network->mutex));
    uint64_t incarnation = network->view[network->own].incarnation;
    checked(pthread_mutex_unlock(&network->mutex));
    sale_record existing;
    int present = chain_lookup(journal, route.sale.id, &existing);
    if (present && (existing.amount != route.sale.amount || strcmp(existing.stamp, route.sale.stamp))) return 2;
    if (!present && !route_eligible(&route, journal->store_id, incarnation, request->normal))
        return merge_record(network, &route, 0, 0) < 0 ? -1 : 3;
    int result = chain_receive(journal, &route.sale);
    if (result < 0 || result == 2) return result;
    return merge_record(network, &route, 1, 0) < 0 ? -1 : result;
}

int network_advance(network_runtime *network, const work_request *request)
{
    size_t position = route_log_find(&network->routes, request->sale_csv);
    if (position == SIZE_MAX) return -1;
    sale_route route = network->routes.records[position];
    if (route.version == request->route_version && request->target == route_first_pending(&route) &&
        request->target < route.count && !strcmp(route.sale.origin, network->config.store_id)) {
        if (route.version == UINT64_MAX) { errno = EOVERFLOW; return -1; }
        route.targets[request->target].state = request->progress; route.version++;
        if (route_log_put(&network->routes, &route) != 0 || merge_record(network, &route, 1, 0) < 0) return -1;
        display_notice(request->progress == 'A' ? "Venta %s compartida con %s.\n" :
                        "Venta %s: %s no disponible; se conserva la entrega pendiente.\n",
                route.sale.id, route.targets[request->target].id);
    }
    checked(pthread_mutex_lock(&network->mutex));
    size_t shared = record_find(network, route.sale.id);
    if (shared != SIZE_MAX) { network->records[shared].control_pending = 0; network->records[shared].target_since = 0; }
    checked(pthread_mutex_unlock(&network->mutex)); network_notify(network); return 0;
}

void network_reply(network_runtime *network, const work_request *request, const char *id, int result)
{
    if (!request->connection || request->connection_slot >= MAX_CONNECTIONS) return;
    checked(pthread_mutex_lock(&network->mutex));
    reply_slot *reply = &network->replies[request->connection_slot];
    reply->token = request->connection; strcpy(reply->id, id);
    reply->code = result == 2 ? REJECT : result == 3 ? DEFER : ACK;
    checked(pthread_mutex_unlock(&network->mutex)); network_notify(network);
}

static void frame(peer_connection *connection, unsigned char type, const void *body, size_t length)
{
    memcpy(connection->tx, "BLK6", 4); connection->tx[4] = 1; connection->tx[5] = type;
    connection->tx[6] = connection->tx[7] = 0;
    uint32_t size = htonl((uint32_t)length); memcpy(connection->tx + 8, &size, 4);
    if (length) memcpy(connection->tx + HEADER, body, length);
    connection->tx_length = HEADER + length; connection->tx_sent = 0;
}
static int notice_add(network_runtime *network, const char *id, const char *text, int show)
{
    for (size_t i = 0; i < network->notice_count; i++) if (!strcmp(network->notices[i].id, id)) return 0;
    if (strlen(id) >= sizeof network->notices[0].id || strlen(text) >= sizeof network->notices[0].text) return -1;
    if (network->notice_count == network->notice_capacity) {
        size_t capacity = network->notice_capacity ? network->notice_capacity * 2 : 32;
        if (capacity < network->notice_capacity || capacity > SIZE_MAX / sizeof *network->notices) return -1;
        chain_notice *notices = realloc(network->notices, capacity * sizeof *notices);
        if (!notices) return -1;
        network->notices = notices; network->notice_capacity = capacity;
    }
    chain_notice *notice = &network->notices[network->notice_count++]; strcpy(notice->id, id); strcpy(notice->text, text);
    if (show) display_notice("Aviso de cadena %s: %s\n", network->config.chain_id, text);
    return 0;
}
static void announce_loss(network_runtime *network, size_t peer, const char *reason)
{
    char id[96], text[160];
    network->notice_sequence++;
    snprintf(id, sizeof id, "%s:%" PRIu64 ":%" PRIu64, network->config.store_id,
             network->book.members[network->book.own].incarnation, network->notice_sequence);
    snprintf(text, sizeof text, "%s perdió su conexión con %s.", network->config.store_id, network->book.members[peer].peer.id);
    display_notice("Se perdió la conexión con la tienda %s en la cadena %s (%s).\n",
            network->book.members[peer].peer.id, network->config.chain_id, reason);
    if (notice_add(network, id, text, 0) < 0) fatal_network(network, "Guardar aviso de cadena");
}
static void disconnect(network_runtime *network, size_t slot, const char *reason, int announce)
{
    peer_connection *connection = &network->connections[slot];
    size_t peer = connection->peer; int was_active = peer < network->book.count && network->active[peer] == (int)slot;
    if (connection->fd >= 0) close(connection->fd);
    if (was_active) {
        network->active[peer] = -1; network->retry[peer] = milliseconds() + network->config.timeout_ms;
        if (connection->ready && announce) network->lost_pending[peer] = 1;
    }
    *connection = (peer_connection){ .fd = -1, .peer = SIZE_MAX };
    if (peer < network->book.count && network->lost_pending[peer]) {
        int candidate = 0;
        for (size_t i = 0; i < MAX_CONNECTIONS; i++)
            if (network->connections[i].fd >= 0 && network->connections[i].peer == peer) candidate = 1;
        if (!candidate && !stopping(network)) { announce_loss(network, peer, reason); network->lost_pending[peer] = 0; }
    }
    refresh_view(network);
}

static int connection_add(network_runtime *network, int fd, size_t peer, int outgoing, int connecting)
{
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) if (network->connections[i].fd < 0) {
        if (nonblocking(fd) < 0) { close(fd); return -1; }
        if (network->next_token == UINT64_MAX) { close(fd); return -1; }
        int64_t now = milliseconds();
        network->connections[i] = (peer_connection){ .fd = fd, .peer = peer, .outgoing = outgoing, .connecting = connecting,
            .token = ++network->next_token, .started = now, .last_rx = now, .last_tx = now, .notice_cursor = network->notice_count };
        return 0;
    }
    close(fd); errno = ENOSPC; return -1;
}
static int hello(network_runtime *network, size_t slot, const char *body)
{
    peer_connection *connection = &network->connections[slot];
    char chain[33], id[33], limit[21], rest[160], extra;
    if (sscanf(body, "%32s %32s %20s", chain, id, limit) != 3 || strcmp(chain, network->config.chain_id) ||
        !strcmp(id, network->config.store_id) || !sale_identifier(id)) return -1;
    for (char *p = limit; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end; intmax_t value = strtoimax(limit, &end, 10);
    if (errno || *end || value <= 0 || value > INT64_MAX) return -1;
    int fields = sscanf(body, "%32s %32s %20s %159[^\n]", chain, id, limit, rest);
    chain_member member;
    if (fields == 3) {
        if (network->book.count != 2 || sscanf(body, "%32s %32s %20s %c", chain, id, limit, &extra) != 3) return -1;
        size_t known = membership_find(&network->book, id); if (known == SIZE_MAX) return -1;
        member = network->book.members[known]; member.incarnation = member.incarnation ? member.incarnation : 1;
        member.ticket = member.ticket ? member.ticket : 1; member.provisional = 0; connection->legacy = 1;
    } else {
        char address[16], port[6], incarnation[21], ticket[21], provisional[2], observed[21];
        int parts = sscanf(rest, "%15s %5s %20s %20s %1s %20s %c", address, port,
                           incarnation, ticket, provisional, observed, &extra);
        if (parts != 5 && parts != 6) return -1;
        char text[200];
        int n = snprintf(text, sizeof text, "%s %s %s %s %s %s", id, address, port, incarnation, ticket, provisional);
        if (n < 0 || (size_t)n >= sizeof text || membership_parse(text, &member) < 0) return -1;
        if (parts == 6) {
            for (char *p = observed; *p; p++) if (*p < '0' || *p > '9') return -1;
            errno = 0; uintmax_t clock = strtoumax(observed, &end, 10);
            if (errno || *end || clock < member.ticket || clock > UINT64_MAX) return -1;
            if (clock > network->book.clock) network->book.clock = (uint64_t)clock;
        }
    }
    if (connection->peer != SIZE_MAX && strcmp(network->book.members[connection->peer].peer.id, id)) return -1;
    if (membership_update(&network->book, &member) < 0 || membership_finalize(&network->book) < 0) return -1;
    size_t peer = membership_find(&network->book, id);
    int old = network->active[peer];
    if (old >= 0 && old != (int)slot) {
        int canonical = strcmp(network->config.store_id, id) < 0;
        if (connection->outgoing != canonical || network->connections[old].outgoing == canonical) return -2;
        disconnect(network, (size_t)old, "conexión duplicada sustituida", 0);
    }
    connection->peer = peer; connection->ready = 1; connection->incarnation = member.incarnation;
    network->active[peer] = (int)slot; refresh_view(network);
    network->lost_pending[peer] = 0;
    if (old < 0) display_notice("Conexión de cadena establecida con %s.\n", id);
    return 0;
}

static void mark_progress(network_runtime *network, size_t index, size_t target, char event)
{
    shared_route *record = &network->records[index];
    if (!record->control_pending) {
        record->control_pending = 1; record->event = event; record->event_target = target;
        record->event_version = record->route.version;
    }
}
static int handle(network_runtime *network, size_t slot, unsigned char type, char *body)
{
    peer_connection *connection = &network->connections[slot];
    if (type == HELLO) return connection->ready ? -1 : hello(network, slot, body);
    if (!connection->ready) return -1;
    if (type == ACK || type == DEFER) {
        if (!connection->waiting_ack || strcmp(body, connection->outstanding)) return -1;
        checked(pthread_mutex_lock(&network->mutex));
        size_t found = record_find(network, body);
        if (found != SIZE_MAX) {
            shared_route *record = &network->records[found];
            record->ack_token[connection->peer] = connection->token;
            record->ack_version[connection->peer] = connection->outstanding_version;
            if (connection->outstanding_normal && record->route.version == connection->outstanding_version)
                mark_progress(network, found, connection->outstanding_target, type == ACK ? 'A' : 'S');
        }
        checked(pthread_mutex_unlock(&network->mutex)); connection->waiting_ack = 0;
    } else if (type == SALE || type == RECOVER) {
        sale_route route;
        if (connection->waiting_worker || connection->pending) return -1;
        if (!strncmp(body, "R7|", 3)) { if (route_decode(body, &route) < 0) return -1; }
        else {
            if (!connection->legacy || network->book.count != 2) return -1;
            route = (sale_route){ .version = 1 };
            if (sale_decode(body, &route.sale) < 0) return -1;
        }
        if (membership_find(&network->book, route.sale.origin) == SIZE_MAX ||
            (type == SALE && !connection->legacy && strcmp(route.sale.origin, network->book.members[connection->peer].peer.id))) return -1;
        work_request *request = &connection->incoming;
        *request = (work_request){ .kind = REQUEST_REMOTE, .connection = connection->token,
            .connection_slot = (unsigned int)slot, .normal = type == SALE && !connection->legacy };
        strcpy(request->sender, network->book.members[connection->peer].peer.id);
        if (route_encode(&route, request->route_wire, sizeof request->route_wire) < 0 ||
            sale_encode(&route.sale, request->sale_csv, sizeof request->sale_csv) < 0) return -1;
        connection->pending = 1;
    } else if (type == MEMBERS) {
        char *save = NULL;
        for (char *line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            chain_member member;
            if (membership_parse(line, &member) < 0) return -1;
            if (!strcmp(member.peer.id, network->config.store_id)) continue;
            if (membership_update(&network->book, &member) < 0) return -1;
        }
        refresh_view(network);
    } else if (type == NOTICE) {
        char *separator = strchr(body, '|'); if (!separator) return -1; *separator++ = '\0';
        if (strchr(separator, '\n') || notice_add(network, body, separator, 1) < 0) return -1;
    } else if (type == BYE) {
        if (strcmp(body, network->book.members[connection->peer].peer.id)) return -1;
        connection->leaving = 1;
    } else if (type == PING || type == PONG) {
        if (*body) return -1;
        if (type == PING) connection->pong = 1;
    } else return -1;
    return 0;
}

static void prepare_output(network_runtime *network, size_t slot)
{
    peer_connection *connection = &network->connections[slot];
    if (connection->fd < 0 || connection->connecting || connection->tx_sent < connection->tx_length) return;
    if (!connection->hello_sent) {
        char member[160], body[256];
        if (membership_format(&network->book.members[network->book.own], member, sizeof member) < 0) return;
        char *rest = strchr(member, ' ');
        int n = snprintf(body, sizeof body, "%s %s %" PRId64 " %s %" PRIu64, network->config.chain_id,
                         network->config.store_id, network->config.limit_cent, rest + 1, network->book.clock);
        frame(connection, HELLO, body, (size_t)n); connection->hello_sent = 1; return;
    }
    if (!connection->ready) return;
    checked(pthread_mutex_lock(&network->mutex));
    reply_slot *reply = &network->replies[slot];
    if (reply->code && reply->token == connection->token) {
        frame(connection, (unsigned char)reply->code, reply->id, strlen(reply->id));
        reply->code = 0; connection->waiting_worker = 0;
        checked(pthread_mutex_unlock(&network->mutex)); return;
    }
    checked(pthread_mutex_unlock(&network->mutex));
    if (connection->pong) { frame(connection, PONG, NULL, 0); connection->pong = 0; return; }
    if (!connection->legacy && connection->notice_cursor < network->notice_count) {
        const chain_notice *notice = &network->notices[connection->notice_cursor++];
        char body[300]; int n = snprintf(body, sizeof body, "%s|%s", notice->id, notice->text);
        frame(connection, NOTICE, body, (size_t)n); return;
    }
    if (!connection->legacy && connection->sent_revision != network->book.revision) {
        char body[ROUTE_WIRE_SIZE]; size_t used = 0;
        for (size_t i = 0; i < network->book.count; i++) if (network->book.members[i].incarnation) {
            int n = membership_format(&network->book.members[i], body + used, sizeof body - used);
            if (n < 0 || (size_t)n + 1 >= sizeof body - used) { fatal_network(network, "Serializar catálogo"); return; }
            used += (size_t)n; body[used++] = '\n';
        }
        frame(connection, MEMBERS, body, used); connection->sent_revision = network->book.revision; return;
    }
    if (!connection->waiting_ack) {
        checked(pthread_mutex_lock(&network->mutex));
        int64_t now = milliseconds();
        for (size_t i = 0; i < network->count; i++) {
            shared_route *record = &network->records[i]; const sale_route *route = &record->route;
            if (!record->applied || record->ready_at > now || record->control_pending) continue;
            if (record->ack_token[connection->peer] == connection->token &&
                record->ack_version[connection->peer] >= route->version) continue;
            size_t target = route_first_pending(route);
            chain_member *peer = &network->book.members[connection->peer];
            int normal = !strcmp(route->sale.origin, network->config.store_id) && target < route->count &&
                         !strcmp(route->targets[target].id, peer->peer.id) &&
                         route->targets[target].incarnation == peer->incarnation;
            if (!normal && !route_eligible(route, peer->peer.id, peer->incarnation, 0)) continue;
            char body[ROUTE_WIRE_SIZE];
            int n = connection->legacy ? sale_encode(&route->sale, body, sizeof body) : route_encode(route, body, sizeof body);
            if (n < 0) { checked(pthread_mutex_unlock(&network->mutex)); fatal_network(network, "Serializar recorrido"); return; }
            frame(connection, normal || connection->legacy ? SALE : RECOVER, body, (size_t)n);
            strcpy(connection->outstanding, route->sale.id);
            connection->outstanding_version = route->version; connection->outstanding_normal = normal;
            connection->outstanding_target = target; connection->waiting_ack = 1; connection->sent_at = now;
            if (normal && !record->target_since) record->target_since = now;
            checked(pthread_mutex_unlock(&network->mutex)); return;
        }
        checked(pthread_mutex_unlock(&network->mutex));
    }
    int64_t interval = network->config.timeout_ms / 2; if (!interval) interval = 1;
    if (milliseconds() - connection->last_tx >= interval) frame(connection, PING, NULL, 0);
}

static int process_input(network_runtime *network, size_t slot)
{
    peer_connection *connection = &network->connections[slot];
    while (!connection->pending && !connection->waiting_worker && connection->rx_length >= HEADER) {
        uint32_t size; memcpy(&size, connection->rx + 8, 4); size = ntohl(size);
        if (memcmp(connection->rx, "BLK6", 4) || connection->rx[4] != 1 || connection->rx[6] || connection->rx[7] ||
            size >= ROUTE_WIRE_SIZE) return -1;
        size_t length = HEADER + (size_t)size;
        if (connection->rx_length < length) break;
        char body[ROUTE_WIRE_SIZE]; memcpy(body, connection->rx + HEADER, size); body[size] = '\0';
        if (memchr(body, '\0', size)) return -1;
        int result = handle(network, slot, connection->rx[5], body);
        if (result < 0) return result;
        memmove(connection->rx, connection->rx + length, connection->rx_length - length);
        connection->rx_length -= length;
    }
    if (connection->pending) {
        int result = queue_try_put(network->queue, connection->incoming);
        if (result == 1) return -2;
        if (!result) { connection->pending = 0; connection->waiting_worker = 1; }
    }
    return 0;
}

static void schedule_controls(network_runtime *network)
{
    int64_t now = milliseconds();
    checked(pthread_mutex_lock(&network->mutex));
    for (size_t i = 0; i < network->count; i++) {
        shared_route *record = &network->records[i]; sale_route *route = &record->route;
        if (!record->applied && !record->restore_queued) {
            work_request request = { .kind = REQUEST_REMOTE };
            if (route_encode(route, request.route_wire, sizeof request.route_wire) >= 0 &&
                sale_encode(&route->sale, request.sale_csv, sizeof request.sale_csv) >= 0 && !queue_try_put(network->queue, request))
                record->restore_queued = 1;
        }
        size_t target = route_first_pending(route);
        if (record->applied && !record->control_pending && record->ready_at <= now &&
            !strcmp(route->sale.origin, network->config.store_id) && target < route->count) {
            size_t peer = membership_find(&network->book, route->targets[target].id);
            int changed = peer != SIZE_MAX && network->book.members[peer].incarnation != route->targets[target].incarnation;
            int online = peer != SIZE_MAX && network->online[peer];
            if (!record->target_since) record->target_since = now;
            if (changed || (!online && now - record->target_since >= network->config.timeout_ms)) mark_progress(network, i, target, 'S');
        }
        if (record->control_pending == 1) {
            work_request request = { .kind = REQUEST_PROGRESS, .target = record->event_target,
                .route_version = record->event_version, .progress = record->event };
            strcpy(request.sale_csv, route->sale.id);
            if (!queue_try_put(network->queue, request)) record->control_pending = 2;
        }
    }
    checked(pthread_mutex_unlock(&network->mutex));
}

static void dial_peers(network_runtime *network)
{
    int64_t now = milliseconds();
    for (size_t peer = 0; peer < network->book.count; peer++) {
        if (peer == network->book.own || network->retry[peer] > now) continue;
        int canonical = strcmp(network->config.store_id, network->book.members[peer].peer.id) < 0;
        int active = network->active[peer];
        if (active >= 0) continue;
        /* Un contacto inicial también puede arrancar desde el ID mayor. */
        if (!canonical && network->book.members[peer].incarnation) continue;
        int exists = 0;
        for (size_t i = 0; i < MAX_CONNECTIONS; i++)
            if (network->connections[i].fd >= 0 && network->connections[i].outgoing && network->connections[i].peer == peer) exists = 1;
        if (exists) continue;
        network->retry[peer] = now + network->config.timeout_ms;
        int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) continue;
        if (nonblocking(fd) < 0) { close(fd); continue; }
        struct sockaddr_in address = { .sin_family = AF_INET,
            .sin_port = htons((uint16_t)network->book.members[peer].peer.port) };
        if (inet_pton(AF_INET, network->book.members[peer].peer.address, &address.sin_addr) != 1) { close(fd); continue; }
        int result = connect(fd, (struct sockaddr *)&address, sizeof address);
        if (result < 0 && errno != EINPROGRESS) { close(fd); continue; }
        connection_add(network, fd, peer, 1, result < 0);
    }
}

static void farewell(network_runtime *network)
{
    /* Aviso de salida de mejor esfuerzo, sin intercalar bytes dentro de otro mensaje. */
    int64_t end = milliseconds() + network->config.timeout_ms;
    int queued[MAX_CONNECTIONS] = {0};
    for (;;) {
        struct pollfd descriptors[MAX_CONNECTIONS]; int pending = 0;
        for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
            peer_connection *connection = &network->connections[i];
            if (connection->fd >= 0 && connection->ready && !connection->legacy && !queued[i] &&
                connection->tx_sent == connection->tx_length) {
                frame(connection, BYE, network->config.store_id, strlen(network->config.store_id)); queued[i] = 1;
            }
            descriptors[i] = (struct pollfd){ .fd = connection->fd, .events = 0 };
            if (connection->fd >= 0 && connection->ready && !connection->legacy &&
                (!queued[i] || connection->tx_sent < connection->tx_length)) { descriptors[i].events = POLLOUT; pending = 1; }
        }
        if (!pending || milliseconds() >= end) return;
        int64_t remaining = end - milliseconds(); if (remaining <= 0) return;
        int n = poll(descriptors, MAX_CONNECTIONS, (int)remaining);
        if (n < 0) { if (errno == EINTR) continue; return; }
        for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
            if (descriptors[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                disconnect(network, i, "salida local", 0); continue;
            }
            if (!(descriptors[i].revents & POLLOUT)) continue;
            peer_connection *connection = &network->connections[i];
            ssize_t sent = send(connection->fd, connection->tx + connection->tx_sent,
                                connection->tx_length - connection->tx_sent, MSG_NOSIGNAL);
            if (sent > 0) connection->tx_sent += (size_t)sent;
            else if (!sent || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
                disconnect(network, i, "salida local", 0);
        }
    }
}

static void *network_loop(void *argument)
{
    network_runtime *network = argument;
    int interval = (int)(network->config.timeout_ms / 2); if (!interval) interval = 1;
    while (!stopping(network)) {
        dial_peers(network); schedule_controls(network);
        struct pollfd descriptors[MAX_CONNECTIONS + 2]; uint64_t tokens[MAX_CONNECTIONS];
        descriptors[0] = (struct pollfd){ .fd = network->listener, .events = POLLIN };
        descriptors[1] = (struct pollfd){ .fd = network->wake[0], .events = POLLIN };
        for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
            peer_connection *connection = &network->connections[i];
            if (connection->fd >= 0) {
                int result = process_input(network, i);
                if (result < 0) {
                    if (result != -2) display_notice("Mensaje de red rechazado; se reinicia la conexión.\n");
                    disconnect(network, i, "mensaje inválido", result != -2);
                } else prepare_output(network, i);
            }
            tokens[i] = connection->token;
            descriptors[i + 2] = (struct pollfd){ .fd = connection->fd, .events = 0 };
            if (connection->fd >= 0) {
                if (connection->connecting || connection->tx_sent < connection->tx_length) descriptors[i + 2].events |= POLLOUT;
                if (!connection->connecting && !connection->pending && !connection->waiting_worker &&
                    connection->rx_length < sizeof connection->rx) descriptors[i + 2].events |= POLLIN;
            }
        }
        int wait_ms = interval;
        for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
            peer_connection *connection = &network->connections[i];
            if (connection->fd >= 0 && !connection->pending && !connection->waiting_worker && connection->rx_length >= HEADER) {
                uint32_t size; memcpy(&size, connection->rx + 8, 4); size = ntohl(size);
                if (size < ROUTE_WIRE_SIZE && connection->rx_length >= HEADER + size) wait_ms = 0;
            }
        }
        int n = poll(descriptors, MAX_CONNECTIONS + 2, wait_ms);
        if (n < 0) { if (errno == EINTR) continue; fatal_network(network, "Esperar red"); break; }
        if (descriptors[1].revents & POLLIN) drain(network->wake[0]);
        if (stopping(network)) break;
        if (descriptors[0].revents & (POLLERR | POLLNVAL)) { fatal_network(network, "Escuchar red"); break; }
        if (descriptors[0].revents & POLLIN) {
            int fd = accept(network->listener, NULL, NULL);
            if (fd >= 0) connection_add(network, fd, SIZE_MAX, 0, 0);
        }
        int64_t now = milliseconds();
        for (size_t i = 0; i < MAX_CONNECTIONS; i++) {
            peer_connection *connection = &network->connections[i];
            if (connection->fd < 0 || connection->token != tokens[i]) continue;
            short events = descriptors[i + 2].revents;
            if (events & (POLLERR | POLLNVAL)) { disconnect(network, i, "error de transporte", 1); continue; }
            if (connection->connecting && events & POLLOUT) {
                int error = 0; socklen_t size = sizeof error;
                if (getsockopt(connection->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error) {
                    disconnect(network, i, "conexión no disponible", 1); continue;
                }
                connection->connecting = 0;
            }
            if (!connection->connecting && events & POLLOUT && connection->tx_sent < connection->tx_length) {
                ssize_t sent = send(connection->fd, connection->tx + connection->tx_sent,
                                    connection->tx_length - connection->tx_sent, MSG_NOSIGNAL);
                if (sent > 0) { connection->tx_sent += (size_t)sent; connection->last_tx = milliseconds(); }
                else if (!sent || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    disconnect(network, i, "fallo de envío", 1); continue;
                }
            }
            if (!connection->connecting && events & (POLLIN | POLLHUP)) {
                ssize_t received = recv(connection->fd, connection->rx + connection->rx_length,
                                        sizeof connection->rx - connection->rx_length, 0);
                if (received > 0) { connection->rx_length += (size_t)received; connection->last_rx = milliseconds(); }
                else if (!received || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    disconnect(network, i, connection->leaving ? "salida normal" : "el par cerró la conexión", 1); continue;
                }
            }
            int64_t timeout = network->config.timeout_ms;
            if ((!connection->ready && now - connection->started > timeout) ||
                (connection->waiting_ack && now - connection->sent_at > timeout) || now - connection->last_rx > timeout * 3)
                disconnect(network, i, "plazo de respuesta agotado", 1);
        }
    }
    farewell(network);
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) if (network->connections[i].fd >= 0)
        disconnect(network, i, "salida local", 0);
    return NULL;
}

void network_status(network_runtime *network)
{
    if (!network) return;
    checked(pthread_mutex_lock(&network->mutex));
    for (size_t i = 0; i < network->member_count; i++) {
        const chain_member *member = &network->view[i];
        if (i == network->own) {
            fprintf(display_output(), "Turno propio: %" PRIu64 "%s\n\n", member->ticket,
                   member->provisional ? " (pendiente de intercambio)" : "");
            continue;
        }
        size_t pending = 0;
        uint64_t token = network->online_token[i];
        for (size_t j = 0; j < network->count; j++) if (network->records[j].applied &&
            (!token || network->records[j].ack_token[i] != token || network->records[j].ack_version[i] < network->records[j].route.version)) pending++;
        fprintf(display_output(), "Par: %s | Red: %s\nEntregas sin confirmar en esta conexión: %zu | Turno: %" PRIu64 "\n\n",
               member->peer.id, network->online[i] ? "CONECTADA" : "DESCONECTADA", pending, member->ticket);
    }
    chain_member ordered[PEERS_MAX]; size_t count = 0;
    for (size_t i = 0; i < network->member_count; i++) if (network->online[i]) ordered[count++] = network->view[i];
    for (size_t i = 1; i < count; i++) {
        chain_member value = ordered[i]; size_t j = i;
        while (j && (ordered[j - 1].ticket > value.ticket ||
               (ordered[j - 1].ticket == value.ticket && strcmp(ordered[j - 1].peer.id, value.peer.id) > 0))) {
            ordered[j] = ordered[j - 1]; j--;
        }
        ordered[j] = value;
    }
    fputs("Orden de incorporación conocido: ", display_output());
    for (size_t i = 0; i < count; i++) fprintf(display_output(), "%s%s", i ? ", " : "", ordered[i].peer.id);
    fputc('\n', display_output());
    checked(pthread_mutex_unlock(&network->mutex));
}

network_runtime *network_start(const store_config *config, const sales_journal *journal, work_queue *queue, int console_wake)
{
    network_runtime *network = calloc(1, sizeof *network); if (!network) return NULL;
    network->listener = network->wake[0] = network->wake[1] = network->book.fd = network->routes.fd = -1;
    network->config = *config; network->queue = queue; network->console_wake = console_wake;
    for (size_t i = 0; i < MAX_CONNECTIONS; i++) network->connections[i] = (peer_connection){ .fd = -1, .peer = SIZE_MAX };
    for (size_t i = 0; i < PEERS_MAX; i++) network->active[i] = -1;
    int result = pthread_mutex_init(&network->mutex, NULL);
    if (result) { free(network); errno = result; return NULL; }
    if (membership_open(&network->book, config) < 0 || route_log_open(&network->routes, config->data_dir) < 0) goto failure;
    if (journal->fresh_history && network->routes.count) {
        int dir = open(config->data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) goto failure;
        char backup[80];
        snprintf(backup, sizeof backup, "routes.previous.%" PRIu64 ".log", network->book.members[network->book.own].incarnation);
        struct stat info;
        if (fstatat(dir, backup, &info, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT) { close(dir); errno = EEXIST; goto failure; }
        route_log_close(&network->routes);
        int result_archive = renameat(dir, "routes.log", dir, backup);
        if (result_archive == 0) result_archive = control_sync(dir);
        if (close(dir) < 0) result_archive = -1;
        if (result_archive < 0 || route_log_open(&network->routes, config->data_dir) < 0) goto failure;
    }
    refresh_view(network);
    if (pipe(network->wake) < 0 || nonblocking(network->wake[0]) < 0 || nonblocking(network->wake[1]) < 0) goto failure;
    for (size_t i = 0; i < network->routes.count; i++) {
        sale_route *route = &network->routes.records[i];
        if (merge_record(network, route, chain_lookup(journal, route->sale.id, NULL), 0) < 0) goto failure;
    }
    for (size_t i = 0; i < journal->count; i++) {
        sale_record sale;
        if (chain_record(journal, i, &sale) < 0 || network_publish(network, &sale, 0) < 0) goto failure;
    }
    network->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (network->listener < 0 || nonblocking(network->listener) < 0) goto failure;
    int reuse = 1;
    if (setsockopt(network->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0) goto failure;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)config->port) };
    if (inet_pton(AF_INET, config->bind_address, &address.sin_addr) != 1 ||
        bind(network->listener, (struct sockaddr *)&address, sizeof address) < 0 || listen(network->listener, PEERS_MAX) < 0) goto failure;
    result = pthread_create(&network->thread, NULL, network_loop, network);
    if (result) { errno = result; goto failure; }
    return network;
failure:
    display_perror("Iniciar red de tienda");
    if (network->listener >= 0) close(network->listener);
    if (network->wake[0] >= 0) close(network->wake[0]);
    if (network->wake[1] >= 0) close(network->wake[1]);
    membership_close(&network->book); route_log_close(&network->routes);
    checked(pthread_mutex_destroy(&network->mutex)); free(network->records); free(network->notices); free(network);
    return NULL;
}
void network_quiesce(network_runtime *network)
{
    if (!network || network->joined) return;
    checked(pthread_mutex_lock(&network->mutex)); network->stopping = 1; checked(pthread_mutex_unlock(&network->mutex));
    network_notify(network); checked(pthread_join(network->thread, NULL)); network->joined = 1;
}
void network_destroy(network_runtime *network)
{
    if (!network) return;
    network_quiesce(network);
    close(network->listener); close(network->wake[0]); close(network->wake[1]);
    membership_close(&network->book); route_log_close(&network->routes);
    checked(pthread_mutex_destroy(&network->mutex)); free(network->records); free(network->notices); free(network);
}
