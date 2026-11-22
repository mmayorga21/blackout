#include "query.h"
#include "query_wire.h"
#include "display.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum { QUERY_CLIENTS = 16 };
typedef struct {
    int fd, queued;
    uint64_t token;
    int64_t started;
    size_t used, sent, length;
    unsigned char input[QUERY_REQUEST_SIZE + 1], output[QUERY_HEADER + REPORT_WIRE_SIZE];
} query_client;
typedef struct { uint64_t token; size_t length; char wire[REPORT_WIRE_SIZE]; } query_response;
struct query_runtime {
    pthread_t thread;
    pthread_mutex_t mutex;
    int listener, wake[2], stopping, joined;
    unsigned int timeout_ms;
    uint64_t next_token;
    work_queue *queue;
    query_client clients[QUERY_CLIENTS]; /* Solo el servidor modifica sockets. */
    query_response responses[QUERY_CLIENTS]; /* Trabajador -> servidor. */
};
static void checked(int result) { if (result) exit(EXIT_FAILURE); }
void query_notify(query_runtime *query)
{
    if (!query) return;
    ssize_t n;
    do { n = write(query->wake[1], "q", 1); } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) display_perror("Notificar consultas");
}
void query_reply(query_runtime *query, const work_request *request, const store_report *report)
{
    if (!query || request->connection_slot >= QUERY_CLIENTS) return;
    char wire[REPORT_WIRE_SIZE]; int length = report_encode(report, wire, sizeof wire);
    if (length < 0) return;
    checked(pthread_mutex_lock(&query->mutex));
    query_response *response = &query->responses[request->connection_slot];
    response->token = request->connection; response->length = (size_t)length;
    memcpy(response->wire, wire, (size_t)length);
    checked(pthread_mutex_unlock(&query->mutex)); query_notify(query);
}
static int stopping(query_runtime *query)
{
    checked(pthread_mutex_lock(&query->mutex)); int value = query->stopping;
    checked(pthread_mutex_unlock(&query->mutex)); return value;
}
static void drop(query_client *client)
{
    if (client->fd >= 0) close(client->fd);
    *client = (query_client){ .fd = -1 };
}
static void accept_client(query_runtime *query)
{
    int fd = accept(query->listener, NULL, NULL); if (fd < 0) return;
    if (query_nonblocking(fd) < 0 || query->next_token == UINT64_MAX) { close(fd); return; }
    for (size_t i = 0; i < QUERY_CLIENTS; i++) if (query->clients[i].fd < 0) {
        query->clients[i] = (query_client){ .fd = fd, .token = ++query->next_token, .started = query_milliseconds() };
        return;
    }
    close(fd);
}
static int prepare(query_runtime *query, size_t slot)
{
    query_client *client = &query->clients[slot];
    if (client->used >= QUERY_HEADER && !client->queued) {
        size_t length;
        if (query_length(client->input, 10, &length) < 0 || length != 10 || client->used > QUERY_REQUEST_SIZE) return -1;
        if (client->used == QUERY_REQUEST_SIZE) {
            if (memcmp(client->input + QUERY_HEADER, "CONSULTAR\n", 10)) return -1;
            work_request request = { .kind = REQUEST_QUERY, .connection = client->token, .connection_slot = (unsigned int)slot };
            int result = queue_try_put(query->queue, request);
            if (result == 1) return -1;
            if (!result) client->queued = 1;
        }
    }
    if (client->queued && !client->length) {
        checked(pthread_mutex_lock(&query->mutex));
        query_response *response = &query->responses[slot];
        if (response->token == client->token && response->length) {
            client->length = query_frame(client->output, response->wire, response->length);
            response->length = 0;
        }
        checked(pthread_mutex_unlock(&query->mutex));
    }
    return 0;
}
static void *query_loop(void *argument)
{
    query_runtime *query = argument;
    while (!stopping(query)) {
        struct pollfd descriptors[QUERY_CLIENTS + 2]; uint64_t tokens[QUERY_CLIENTS];
        descriptors[0] = (struct pollfd){ .fd = query->listener, .events = POLLIN };
        descriptors[1] = (struct pollfd){ .fd = query->wake[0], .events = POLLIN };
        int wait = -1;
        int64_t now = query_milliseconds(); if (now < 0) break;
        for (size_t i = 0; i < QUERY_CLIENTS; i++) {
            query_client *client = &query->clients[i];
            if (client->fd >= 0 && (now - client->started >= query->timeout_ms || prepare(query, i) < 0)) drop(client);
            descriptors[i + 2] = (struct pollfd){ .fd = client->fd, .events = 0 };
            tokens[i] = client->token;
            if (client->fd >= 0) {
                if (client->length) descriptors[i + 2].events = POLLOUT;
                else if (client->used < QUERY_REQUEST_SIZE) descriptors[i + 2].events = POLLIN;
                int remaining = (int)(query->timeout_ms - (unsigned int)(now - client->started));
                if (wait < 0 || remaining < wait) wait = remaining;
            }
        }
        int result = poll(descriptors, QUERY_CLIENTS + 2, wait);
        if (result < 0) { if (errno == EINTR) continue; display_perror("Esperar consultas"); break; }
        if (descriptors[1].revents & POLLIN) {
            char data[128]; while (read(query->wake[0], data, sizeof data) > 0) { }
        }
        if (stopping(query)) break;
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; display_perror("Escuchar consultas"); break; }
        if (descriptors[0].revents & POLLIN) accept_client(query);
        for (size_t i = 0; i < QUERY_CLIENTS; i++) {
            query_client *client = &query->clients[i]; short events = descriptors[i + 2].revents;
            if (client->fd < 0 || client->token != tokens[i]) continue;
            if (events & (POLLERR | POLLNVAL)) { drop(client); continue; }
            if (events & POLLOUT) {
                ssize_t n = send(client->fd, client->output + client->sent, client->length - client->sent, MSG_NOSIGNAL);
                if (n > 0) { client->sent += (size_t)n; if (client->sent == client->length) drop(client); }
                else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) drop(client);
            } else if (events & (POLLIN | POLLHUP)) {
                ssize_t n = recv(client->fd, client->input + client->used, sizeof client->input - client->used, 0);
                if (n > 0) client->used += (size_t)n;
                else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) drop(client);
            }
        }
    }
    for (size_t i = 0; i < QUERY_CLIENTS; i++) drop(&query->clients[i]);
    return NULL;
}
query_runtime *query_start(const store_config *config, work_queue *queue)
{
    query_runtime *query = calloc(1, sizeof *query); if (!query) return NULL;
    query->listener = query->wake[0] = query->wake[1] = -1;
    query->queue = queue; query->timeout_ms = config->timeout_ms;
    for (size_t i = 0; i < QUERY_CLIENTS; i++) query->clients[i].fd = -1;
    int result = pthread_mutex_init(&query->mutex, NULL);
    if (result) { free(query); errno = result; return NULL; }
    query->listener = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)config->query_port) };
    if (query->listener < 0 || query_nonblocking(query->listener) < 0 ||
        setsockopt(query->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse) < 0 ||
        inet_pton(AF_INET, config->bind_address, &address.sin_addr) != 1 ||
        bind(query->listener, (struct sockaddr *)&address, sizeof address) < 0 || listen(query->listener, QUERY_CLIENTS) < 0 ||
        pipe(query->wake) < 0 || query_nonblocking(query->wake[0]) < 0 || query_nonblocking(query->wake[1]) < 0) goto failure;
    result = pthread_create(&query->thread, NULL, query_loop, query);
    if (result) { errno = result; goto failure; }
    return query;
failure:
    display_perror("Iniciar consultas de gobierno");
    if (query->listener >= 0) close(query->listener);
    if (query->wake[0] >= 0) close(query->wake[0]);
    if (query->wake[1] >= 0) close(query->wake[1]);
    checked(pthread_mutex_destroy(&query->mutex)); free(query); return NULL;
}
void query_stop(query_runtime *query)
{
    if (!query || query->joined) return;
    checked(pthread_mutex_lock(&query->mutex)); query->stopping = 1;
    checked(pthread_mutex_unlock(&query->mutex)); query_notify(query);
    checked(pthread_join(query->thread, NULL)); query->joined = 1;
}
void query_destroy(query_runtime *query)
{
    if (!query) return;
    query_stop(query);
    close(query->listener); close(query->wake[0]); close(query->wake[1]);
    checked(pthread_mutex_destroy(&query->mutex)); free(query);
}
