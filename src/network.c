#include "network.h"
#include "chain_journal.h"
#include "peers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { WIRE_HEADER = 12, WIRE_MAX = 256, HELLO = 1, SALE = 2, ACK = 3, PING = 4, PONG = 5, REJECT = 6 };
typedef struct {
    int64_t due;
    unsigned char type;
    char body[WIRE_MAX];
} delayed_frame;
struct network_runtime {
    store_config config;
    peer_config peer;
    work_queue *queue;
    int console_wake, wake[2], listener;
    pthread_t thread;
    pthread_mutex_t mutex;
    int stopping, joined, connected;
    sale_record *records;
    size_t count, capacity, acknowledged;
    uint64_t reply_connection;
    char reply_id[SALE_ID_SIZE];
    int reply_code;
    delayed_frame *delayed;
};

typedef struct {
    network_runtime *runtime;
    int socket, connecting, ready, hello_sent, waiting_ack, waiting_worker, pending, pong;
    uint64_t generation;
    int64_t started, last_rx, last_tx, sent_at, retry_at, received_at;
    size_t delayed_head, delayed_count;
    unsigned int peer_delay_ms;
    size_t cursor, rx_length, tx_length, tx_sent;
    unsigned char rx[4096], tx[WIRE_HEADER + WIRE_MAX];
    char outstanding[SALE_ID_SIZE];
    work_request incoming;
} connection_state;

static int stopping(network_runtime *runtime);

static void checked(int result)
{
    if (result) { fprintf(stderr, "Sincronización de red: %s\n", strerror(result)); exit(EXIT_FAILURE); }
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) { perror("Reloj de red"); exit(EXIT_FAILURE); }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void network_notify(network_runtime *network)
{
    if (!network) return;
    ssize_t n;
    do { n = write(network->wake[1], "x", 1); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) { perror("Notificar red"); exit(EXIT_FAILURE); }
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

static int nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? -1 : 0;
}

static void progress(network_runtime *network, int connected, size_t acknowledged)
{
    checked(pthread_mutex_lock(&network->mutex));
    network->connected = connected;
    network->acknowledged = acknowledged;
    checked(pthread_mutex_unlock(&network->mutex));
}

static void disconnect_peer(connection_state *state, const char *reason)
{
    int was_ready = state->ready;
    if (state->socket >= 0) close(state->socket);
    network_runtime *runtime = state->runtime;
    uint64_t generation = state->generation;
    *state = (connection_state){ .runtime = runtime, .socket = -1, .generation = generation,
                               .retry_at = milliseconds() + runtime->config.timeout_ms };
    progress(runtime, 0, 0);
    if (was_ready && !stopping(runtime))
        fprintf(stderr, "Se perdió la conexión con la tienda %s en la cadena %s (%s).\n",
                runtime->peer.id, runtime->config.chain_id, reason);
}

static void establish(connection_state *state, int fd, int connecting)
{
    state->socket = fd;
    state->connecting = connecting;
    state->generation++;
    if (!state->generation) { fputs("Generaciones de conexión agotadas.\n", stderr); exit(EXIT_FAILURE); }
    state->started = state->last_rx = state->last_tx = milliseconds();
}

static void frame(connection_state *state, unsigned char type, const void *body, size_t length)
{
    memcpy(state->tx, "BLK6", 4);
    state->tx[4] = 1; state->tx[5] = type; state->tx[6] = state->tx[7] = 0;
    uint32_t wire_length = htonl((uint32_t)length);
    memcpy(state->tx + 8, &wire_length, 4);
    if (length) memcpy(state->tx + WIRE_HEADER, body, length);
    state->tx_length = WIRE_HEADER + length;
    state->tx_sent = 0;
}

static int snapshot_record(network_runtime *network, size_t index, sale_record *record)
{
    checked(pthread_mutex_lock(&network->mutex));
    int exists = index < network->count;
    if (exists) *record = network->records[index];
    checked(pthread_mutex_unlock(&network->mutex));
    return exists;
}

static void prepare_output(connection_state *state)
{
    if (state->socket < 0 || state->connecting || state->tx_sent < state->tx_length) return;
    network_runtime *runtime = state->runtime;
    if (!state->hello_sent) {
        char hello[128];
        int n = snprintf(hello, sizeof hello, "%s %s %" PRId64 " %u",
                         runtime->config.chain_id, runtime->config.store_id, runtime->config.limit_cent,
                         runtime->config.network_delay_ms);
        frame(state, HELLO, hello, (size_t)n); state->hello_sent = 1; return;
    }
    if (!state->ready) return;
    checked(pthread_mutex_lock(&runtime->mutex));
    if (runtime->reply_code && runtime->reply_connection == state->generation) {
        frame(state, runtime->reply_code == 1 ? ACK : REJECT, runtime->reply_id, strlen(runtime->reply_id));
        runtime->reply_code = 0;
        state->waiting_worker = 0;
        checked(pthread_mutex_unlock(&runtime->mutex));
        return;
    }
    checked(pthread_mutex_unlock(&runtime->mutex));
    if (state->pong) { frame(state, PONG, NULL, 0); state->pong = 0; return; }
    sale_record sale;
    if (!state->waiting_ack && snapshot_record(runtime, state->cursor, &sale)) {
        char csv[SALE_CSV_SIZE];
        int n = sale_encode(&sale, csv, sizeof csv);
        if (n < 0) { fputs("No se pudo serializar una venta de red.\n", stderr); exit(EXIT_FAILURE); }
        frame(state, SALE, csv, (size_t)n);
        strcpy(state->outstanding, sale.id);
        state->waiting_ack = 1; state->sent_at = milliseconds();
        return;
    }
    int64_t interval = ((int64_t)runtime->config.timeout_ms + runtime->config.network_delay_ms + state->peer_delay_ms) / 2;
    if (!interval) interval = 1;
    if (milliseconds() - state->last_tx >= interval) frame(state, PING, NULL, 0);
}

static int valid_hello(connection_state *state, const char *body)
{
    char chain[33], identity[33], limit[21], delay[16], extra;
    int fields = sscanf(body, "%32s %32s %20s %15s %c", chain, identity, limit, delay, &extra);
    if (fields != 3 && fields != 4) return 0;
    for (char *p = limit; *p; p++) if (*p < '0' || *p > '9') return 0;
    errno = 0;
    char *end;
    intmax_t value = strtoimax(limit, &end, 10);
    if (errno || *end || value <= 0 || value > INT64_MAX ||
        strcmp(chain, state->runtime->config.chain_id) || strcmp(identity, state->runtime->peer.id)) return 0;
    unsigned long lag = 0;
    if (fields == 4) {
        for (char *p = delay; *p; p++) if (*p < '0' || *p > '9') return 0;
        errno = 0;
        lag = strtoul(delay, &end, 10);
        if (errno || *end || lag > 60000) return 0;
    }
    state->peer_delay_ms = (unsigned int)lag;
    return 1;
}

static int handle_frame(connection_state *state, unsigned char type, char *body)
{
    if (type == HELLO) {
        if (state->ready || !valid_hello(state, body)) return -1;
        state->ready = 1; progress(state->runtime, 1, state->cursor);
        fprintf(stderr, "Conexión de cadena establecida con %s.\n", state->runtime->peer.id);
        return 0;
    }
    if (!state->ready) return -1;
    if (type == ACK) {
        if (!state->waiting_ack || strcmp(body, state->outstanding)) return -1;
        state->waiting_ack = 0; state->cursor++;
        progress(state->runtime, 1, state->cursor);
    } else if (type == SALE) {
        sale_record sale;
        if (state->waiting_worker || state->pending || sale_decode(body, &sale) < 0 ||
            (strcmp(sale.origin, state->runtime->config.store_id) && strcmp(sale.origin, state->runtime->peer.id))) return -1;
        state->incoming = (work_request){ .kind = REQUEST_REMOTE, .connection = state->generation };
        strcpy(state->incoming.sale_csv, body);
        state->pending = 1;
    } else if (type == PING || type == PONG) {
        if (*body) return -1;
        if (type == PING) state->pong = 1;
    } else return -1;
    return 0;
}

static int process_input(connection_state *state)
{
    size_t capacity = state->runtime->config.queue_capacity;
    /* Reconstruir mensajes ahora, entregarlos al protocolo cuando venza su atraso. */
    while (state->delayed_count < capacity && state->rx_length >= WIRE_HEADER) {
        unsigned char *header = state->rx;
        uint32_t size;
        memcpy(&size, header + 8, 4); size = ntohl(size);
        if (memcmp(header, "BLK6", 4) || header[4] != 1 || header[6] || header[7] || size >= WIRE_MAX) return -1;
        size_t length = WIRE_HEADER + (size_t)size;
        if (state->rx_length < length) break;
        size_t tail = (state->delayed_head + state->delayed_count) % capacity;
        delayed_frame *message = &state->runtime->delayed[tail];
        memcpy(message->body, header + WIRE_HEADER, size); message->body[size] = '\0';
        if (memchr(message->body, '\0', size)) return -1;
        message->type = header[5];
        message->due = state->received_at + state->runtime->config.network_delay_ms;
        state->delayed_count++;
        memmove(state->rx, state->rx + length, state->rx_length - length);
        state->rx_length -= length;
    }
    while (!state->pending && !state->waiting_worker && state->delayed_count) {
        delayed_frame *message = &state->runtime->delayed[state->delayed_head];
        if (message->due > milliseconds()) break;
        if (handle_frame(state, message->type, message->body) < 0) return -1;
        state->delayed_head = (state->delayed_head + 1) % capacity;
        state->delayed_count--;
    }
    if (state->pending) {
        int result = queue_try_put(state->runtime->queue, state->incoming);
        if (result == 1) return -1;
        if (result == 0) { state->pending = 0; state->waiting_worker = 1; }
    }
    return 0;
}

static void dial(connection_state *state)
{
    network_runtime *runtime = state->runtime;
    if (strcmp(runtime->config.store_id, runtime->peer.id) >= 0 || milliseconds() < state->retry_at) return;
    state->retry_at = milliseconds() + runtime->config.timeout_ms;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return;
    if (nonblocking(fd) < 0) { close(fd); return; }
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)runtime->peer.port) };
    if (inet_pton(AF_INET, runtime->peer.address, &address.sin_addr) != 1) { close(fd); return; }
    int result = connect(fd, (struct sockaddr *)&address, sizeof address);
    if (result < 0 && errno != EINPROGRESS) { close(fd); return; }
    establish(state, fd, result < 0);
}

static int stopping(network_runtime *runtime)
{
    checked(pthread_mutex_lock(&runtime->mutex));
    int result = runtime->stopping;
    checked(pthread_mutex_unlock(&runtime->mutex));
    return result;
}

static void *network_loop(void *argument)
{
    network_runtime *runtime = argument;
    connection_state state = { .runtime = runtime, .socket = -1 };
    int interval = (int)(runtime->config.timeout_ms / 2);
    if (!interval) interval = 1;
    while (!stopping(runtime)) {
        if (state.socket < 0) dial(&state);
        if (process_input(&state) < 0) {
            fputs("Mensaje de red rechazado; se reinicia la conexión.\n", stderr);
            disconnect_peer(&state, "mensaje inválido"); continue;
        }
        prepare_output(&state);
        struct pollfd descriptors[3] = {
            { .fd = runtime->listener, .events = POLLIN },
            { .fd = state.socket, .events = 0 },
            { .fd = runtime->wake[0], .events = POLLIN }
        };
        if (state.socket >= 0) {
            if (state.connecting || state.tx_sent < state.tx_length) descriptors[1].events |= POLLOUT;
            if (!state.connecting && !state.pending && !state.waiting_worker &&
                state.delayed_count < runtime->config.queue_capacity && state.rx_length < sizeof state.rx)
                descriptors[1].events |= POLLIN;
        }
        int wait_ms = interval;
        if (state.delayed_count && !state.pending && !state.waiting_worker) {
            int64_t remaining = runtime->delayed[state.delayed_head].due - milliseconds();
            if (remaining < 0) remaining = 0;
            if (remaining < wait_ms) wait_ms = (int)remaining;
        }
        if (state.rx_length >= WIRE_HEADER && state.delayed_count < runtime->config.queue_capacity) {
            uint32_t buffered;
            memcpy(&buffered, state.rx + 8, 4); buffered = ntohl(buffered);
            if (buffered < WIRE_MAX && state.rx_length >= WIRE_HEADER + buffered) wait_ms = 0;
        }
        int n;
        do { n = poll(descriptors, 3, wait_ms); } while (n < 0 && errno == EINTR);
        if (n < 0 || descriptors[0].revents & (POLLERR | POLLNVAL)) {
            perror("Esperar red"); queue_stop(runtime->queue, QUEUE_ERROR);
            ssize_t ignored;
            do { ignored = write(runtime->console_wake, "x", 1); } while (ignored < 0 && errno == EINTR);
            break;
        }
        if (descriptors[2].revents & POLLIN) drain(runtime->wake[0]);
        if (stopping(runtime)) break;
        if (descriptors[0].revents & POLLIN) {
            int accepted = accept(runtime->listener, NULL, NULL);
            if (accepted >= 0) {
                if (state.socket >= 0 || strcmp(runtime->config.store_id, runtime->peer.id) <= 0 || nonblocking(accepted) < 0)
                    close(accepted);
                else establish(&state, accepted, 0);
            }
        }
        if (descriptors[1].fd != state.socket || state.socket < 0) continue;
        short events = descriptors[1].revents;
        if (events & (POLLERR | POLLNVAL)) { disconnect_peer(&state, "error de transporte"); continue; }
        if (state.connecting && events & POLLOUT) {
            int error = 0; socklen_t length = sizeof error;
            if (getsockopt(state.socket, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) {
                disconnect_peer(&state, "conexión no disponible"); continue;
            }
            state.connecting = 0;
        }
        if (!state.connecting && events & POLLOUT && state.tx_sent < state.tx_length) {
            ssize_t sent = send(state.socket, state.tx + state.tx_sent, state.tx_length - state.tx_sent, MSG_NOSIGNAL);
            if (sent > 0) { state.tx_sent += (size_t)sent; state.last_tx = milliseconds(); }
            else if (!sent || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                disconnect_peer(&state, "fallo de envío"); continue;
            }
        }
        if (!state.connecting && events & (POLLIN | POLLHUP)) {
            ssize_t received = recv(state.socket, state.rx + state.rx_length, sizeof state.rx - state.rx_length, 0);
            if (received > 0) {
                state.rx_length += (size_t)received;
                state.received_at = state.last_rx = milliseconds();
            } else if (!received || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                disconnect_peer(&state, !received ? "el par cerró la conexión" : "fallo de recepción"); continue;
            }
        }
        int64_t now = milliseconds(), timeout = runtime->config.timeout_ms;
        int64_t lag = (int64_t)runtime->config.network_delay_ms + state.peer_delay_ms;
        if ((!state.ready && now - state.started > timeout + runtime->config.network_delay_ms) ||
            (state.waiting_ack && now - state.sent_at > timeout + lag) || now - state.last_rx > timeout * 3 + lag)
            disconnect_peer(&state, "plazo de respuesta agotado");
    }
    disconnect_peer(&state, "servicio de red detenido");
    return NULL;
}

int network_publish(network_runtime *network, const sale_record *sale)
{
    if (!network) return 0;
    checked(pthread_mutex_lock(&network->mutex));
    if (!network->stopping) {
        if (network->count == network->capacity) {
            size_t capacity = network->capacity ? network->capacity * 2 : 64;
            if (capacity < network->capacity || capacity > SIZE_MAX / sizeof *network->records) {
                checked(pthread_mutex_unlock(&network->mutex)); errno = ENOMEM; return -1;
            }
            sale_record *records = realloc(network->records, capacity * sizeof *records);
            if (!records) { checked(pthread_mutex_unlock(&network->mutex)); return -1; }
            network->records = records; network->capacity = capacity;
        }
        network->records[network->count++] = *sale;
    }
    checked(pthread_mutex_unlock(&network->mutex));
    network_notify(network);
    return 0;
}

void network_reply(network_runtime *network, uint64_t connection, const char *id, int accepted)
{
    checked(pthread_mutex_lock(&network->mutex));
    network->reply_connection = connection;
    strcpy(network->reply_id, id);
    network->reply_code = accepted ? 1 : 2;
    checked(pthread_mutex_unlock(&network->mutex));
    network_notify(network);
}

void network_status(network_runtime *network)
{
    if (!network) return;
    checked(pthread_mutex_lock(&network->mutex));
    int connected = network->connected;
    size_t pending = network->count - network->acknowledged;
    checked(pthread_mutex_unlock(&network->mutex));
    printf("Par: %s | Red: %s | Entregas sin confirmar en esta conexión: %zu | Atraso de recepción: %u ms\n",
           network->peer.id, connected ? "CONECTADA" : "DESCONECTADA", pending, network->config.network_delay_ms);
}

network_runtime *network_start(const store_config *config, const sales_journal *journal,
                               work_queue *queue, int console_wake)
{
    network_runtime *runtime = calloc(1, sizeof *runtime);
    if (!runtime) return NULL;
    runtime->listener = runtime->wake[0] = runtime->wake[1] = -1;
    runtime->config = *config; runtime->queue = queue; runtime->console_wake = console_wake;
    int result = pthread_mutex_init(&runtime->mutex, NULL);
    if (result) { free(runtime); errno = result; return NULL; }
    runtime->delayed = calloc(config->queue_capacity, sizeof *runtime->delayed);
    if (!runtime->delayed) goto failure;
    if (peers_load(config, &runtime->peer) < 0) goto failure;
    if (pipe(runtime->wake) < 0 || nonblocking(runtime->wake[0]) < 0 || nonblocking(runtime->wake[1]) < 0) goto failure;
    for (size_t i = 0; i < journal->count; i++) {
        sale_record record;
        if (chain_record(journal, i, &record) < 0 || network_publish(runtime, &record) < 0) goto failure;
    }
    runtime->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (runtime->listener < 0 || nonblocking(runtime->listener) < 0) goto failure;
    int reuse = 1;
    if (setsockopt(runtime->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0) goto failure;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)config->port) };
    if (inet_pton(AF_INET, config->bind_address, &address.sin_addr) != 1 ||
        bind(runtime->listener, (struct sockaddr *)&address, sizeof address) < 0 || listen(runtime->listener, 8) < 0) goto failure;
    result = pthread_create(&runtime->thread, NULL, network_loop, runtime);
    if (result) { errno = result; goto failure; }
    return runtime;
failure:
    perror("Iniciar red de tienda");
    if (runtime->listener >= 0) close(runtime->listener);
    if (runtime->wake[0] >= 0) close(runtime->wake[0]);
    if (runtime->wake[1] >= 0) close(runtime->wake[1]);
    checked(pthread_mutex_destroy(&runtime->mutex));
    free(runtime->delayed); free(runtime->records); free(runtime);
    return NULL;
}

void network_quiesce(network_runtime *network)
{
    if (!network || network->joined) return;
    checked(pthread_mutex_lock(&network->mutex)); network->stopping = 1; checked(pthread_mutex_unlock(&network->mutex));
    network_notify(network);
    checked(pthread_join(network->thread, NULL)); network->joined = 1;
}

void network_destroy(network_runtime *network)
{
    if (!network) return;
    network_quiesce(network);
    close(network->listener); close(network->wake[0]); close(network->wake[1]);
    checked(pthread_mutex_destroy(&network->mutex));
    free(network->delayed); free(network->records); free(network);
}
