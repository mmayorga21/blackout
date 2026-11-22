#include "government.h"
#include "query_wire.h"
#include "sale.h"
#include "display.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <inttypes.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int attempted, error;
    government_observation observation;
} government_result;
typedef struct {
    int fd, connecting;
    size_t sent, used, wanted;
    unsigned char input[QUERY_HEADER + REPORT_WIRE_SIZE];
} government_attempt;
static void finish(government_attempt *attempt, government_result *result, int error)
{
    if (attempt->fd >= 0) close(attempt->fd);
    attempt->fd = -1; result->error = error;
}
static int collect(const government_config *config, size_t target, government_result *results)
{
    government_attempt *attempts = calloc(config->count, sizeof *attempts);
    if (!attempts) return -1;
    int64_t started = query_milliseconds();
    if (started < 0) { free(attempts); return -1; }
    int64_t deadline = started + config->timeout_ms;
    unsigned char request[QUERY_REQUEST_SIZE]; query_request(request);
    for (size_t i = 0; i < config->count; i++) {
        government_attempt *attempt = &attempts[i]; attempt->fd = -1;
        if (target != SIZE_MAX && i != target) continue;
        government_result *result = &results[i]; result->attempted = 1;
        attempt->fd = socket(AF_INET, SOCK_STREAM, 0);
        if (attempt->fd < 0 || query_nonblocking(attempt->fd) < 0) { finish(attempt, result, errno); continue; }
        struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)config->stores[i].port) };
        if (inet_pton(AF_INET, config->stores[i].address, &address.sin_addr) != 1) { finish(attempt, result, EINVAL); continue; }
        int connected = connect(attempt->fd, (struct sockaddr *)&address, sizeof address);
        if (connected < 0 && errno != EINPROGRESS) { finish(attempt, result, errno); continue; }
        attempt->connecting = connected < 0;
    }
    for (;;) {
        struct pollfd descriptors[GOVERNMENT_STORES]; int active = 0;
        for (size_t i = 0; i < config->count; i++) {
            government_attempt *attempt = &attempts[i];
            descriptors[i] = (struct pollfd){ .fd = attempt->fd, .events = 0 };
            if (attempt->fd >= 0) {
                descriptors[i].events = attempt->connecting || attempt->sent < sizeof request ? POLLOUT : POLLIN;
                active = 1;
            }
        }
        if (!active) break;
        int64_t now = query_milliseconds();
        if (now < 0 || now >= deadline) {
            for (size_t i = 0; i < config->count; i++) if (attempts[i].fd >= 0) finish(&attempts[i], &results[i], now < 0 ? EIO : ETIMEDOUT);
            break;
        }
        int wait = deadline - now > INT_MAX ? INT_MAX : (int)(deadline - now);
        int ready = poll(descriptors, (nfds_t)config->count, wait);
        if (ready < 0) {
            if (errno == EINTR) continue;
            int saved = errno;
            for (size_t i = 0; i < config->count; i++) if (attempts[i].fd >= 0) finish(&attempts[i], &results[i], saved);
            break;
        }
        for (size_t i = 0; i < config->count; i++) {
            government_attempt *attempt = &attempts[i]; government_result *result = &results[i];
            short events = descriptors[i].revents;
            if (attempt->fd < 0 || !events) continue;
            if (attempt->connecting && events & (POLLOUT | POLLERR | POLLHUP)) {
                int error = 0; socklen_t length = sizeof error;
                if (getsockopt(attempt->fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0) error = errno;
                if (error) { finish(attempt, result, error); continue; }
                attempt->connecting = 0;
            }
            if (events & (POLLERR | POLLNVAL)) { finish(attempt, result, EIO); continue; }
            if (events & POLLOUT && attempt->sent < sizeof request) {
                ssize_t n = send(attempt->fd, request + attempt->sent, sizeof request - attempt->sent, MSG_NOSIGNAL);
                if (n > 0) attempt->sent += (size_t)n;
                else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { finish(attempt, result, EIO); continue; }
            }
            if (events & (POLLIN | POLLHUP)) {
                ssize_t n = recv(attempt->fd, attempt->input + attempt->used, sizeof attempt->input - attempt->used, 0);
                if (n > 0) {
                    attempt->used += (size_t)n;
                    if (attempt->used >= QUERY_HEADER && !attempt->wanted) {
                        size_t length;
                        if (query_length(attempt->input, REPORT_WIRE_SIZE - 1, &length) < 0) { finish(attempt, result, EPROTO); continue; }
                        attempt->wanted = QUERY_HEADER + length;
                    }
                    if (attempt->wanted && attempt->used >= attempt->wanted) {
                        size_t length = attempt->wanted - QUERY_HEADER;
                        if (attempt->used != attempt->wanted || memchr(attempt->input + QUERY_HEADER, '\0', length)) { finish(attempt, result, EPROTO); continue; }
                        char wire[REPORT_WIRE_SIZE]; memcpy(wire, attempt->input + QUERY_HEADER, length); wire[length] = '\0';
                        store_report report;
                        if (report_decode(wire, &report) < 0 || strcmp(report.store_id, config->stores[i].id) || strcmp(report.chain_id, config->stores[i].chain)) {
                            finish(attempt, result, EPROTO); continue;
                        }
                        result->observation = (government_observation){ .present = 1, .report = report };
                        if (sale_timestamp(result->observation.received, sizeof result->observation.received) < 0) {
                            result->observation.present = 0; finish(attempt, result, EIO); continue;
                        }
                        time_t observed = time(NULL);
                        if (observed < 0) { result->observation.present = 0; finish(attempt, result, EIO); continue; }
                        result->observation.observed_at = (int64_t)observed;
                        finish(attempt, result, 0);
                    }
                } else if (!n) finish(attempt, result, ECONNRESET);
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) finish(attempt, result, errno);
            }
        }
    }
    free(attempts); return 0;
}
/* 0 fin; 1 línea válida; 2 línea descartada; -1 error. */
static int read_command(char *line, size_t capacity)
{
    size_t used = 0; int invalid = 0;
    for (;;) {
        int c = getchar();
        if (c == EOF) {
            if (ferror(stdin)) return -1;
            if (!used && !invalid) return 0;
            line[used] = '\0'; return invalid ? 2 : 1;
        }
        if (c == '\n') { line[used] = '\0'; return invalid ? 2 : 1; }
        if (!c || used == capacity - 1) invalid = 1;
        else if (!invalid) line[used++] = (char)c;
    }
}
static int execute(const government_config *config, government_cache *cache, size_t target)
{
    government_result *results = calloc(config->count, sizeof *results); if (!results) return -1;
    if (collect(config, target, results) < 0) { free(results); return -1; }
    int failed = 0;
    for (size_t i = 0; i < config->count; i++) if (results[i].attempted) {
        government_result *result = &results[i]; const government_observation *observation;
        display_begin("CONSULTA: %s", config->stores[i].id);
        if (result->observation.present) {
            observation = &result->observation;
            fputs("RESPUESTA ACTUAL (recibida en esta consulta)\n", display_output());
            fprintf(display_output(), "Recibida por el consultor: %s\n", observation->received);
            report_print(&observation->report);
            if (government_cache_save(cache, config, i, observation) < 0) {
                display_perror("Guardar historial de consultas"); failed = 1;
            }
        } else {
            fprintf(display_output(), "Tienda %s: %s\nMotivo: %s\n", config->stores[i].id,
                result->error == EPROTO ? "RESPUESTA INVALIDA" : "SIN RESPUESTA", strerror(result->error));
            observation = &cache->latest[i];
            if (observation->present) {
                fprintf(display_output(), "INFORMACION HISTORICA: no confirma el estado actual.\nÚltima respuesta recibida: %s\n", observation->received);
                time_t now = time(NULL);
                if (now >= 0 && (int64_t)now >= observation->observed_at)
                    fprintf(display_output(), "Antigüedad aproximada: %" PRId64 " segundos\n", (int64_t)now - observation->observed_at);
                else fputs("Antigüedad no estimable con el reloj local actual.\n", display_output());
                report_print(&observation->report);
            } else fputs("No hay información histórica guardada para este destino.\n", display_output());
        }
        display_end();
        if (failed) break;
    }
    free(results); return failed ? -1 : 0;
}
int government_run(const char *path)
{
    government_config *config = calloc(1, sizeof *config);
    government_cache *cache = calloc(1, sizeof *cache);
    if (!config || !cache) { free(config); free(cache); return EXIT_FAILURE; }
    display_begin("INICIO DEL CONSULTOR");
    if (government_config_load(path, config) < 0 || government_cache_open(cache, config) < 0) {
        display_end(); free(config); free(cache); return EXIT_FAILURE;
    }
    display_end();
    display_begin("GOBIERNO");
    fputs("Comandos: consultar TIENDA | consultar todas | ayuda | salir\n", display_output());
    fprintf(display_output(), "Tiendas configuradas: %zu | Historial: %s/consultas.log\n", config->count, config->data_dir);
    display_end();
    int failed = 0, done = 0;
    while (!done) {
        if (isatty(STDIN_FILENO)) { fputs("gobierno> ", stdout); fflush(stdout); }
        char line[256]; int result = read_command(line, sizeof line);
        if (result <= 0) { if (result < 0) failed = 1; break; }
        char *command = line;
        while (isspace((unsigned char)*command)) command++;
        size_t length = strlen(command);
        while (length && isspace((unsigned char)command[length - 1])) command[--length] = '\0';
        if (result == 2) {
            display_begin("COMANDO: entrada inválida");
            fputs("Comando demasiado largo o con bytes inválidos.\n", display_errors()); display_end(); continue;
        }
        if (!*command) continue;
        if (!strcmp(command, "salir")) done = 1;
        else if (!strcmp(command, "ayuda")) {
            display_begin("COMANDO: ayuda");
            fputs("consultar TIENDA | consultar todas | ayuda | salir\nLas respuestas de cadena muestran ingresos conocidos, no un total global garantizado.\n", display_output());
            display_end();
        } else if (!strncmp(command, "consultar", 9) && isspace((unsigned char)command[9])) {
            char *target = command + 9; while (isspace((unsigned char)*target)) target++;
            size_t index = SIZE_MAX;
            for (size_t i = 0; i < config->count; i++) if (!strcmp(config->stores[i].id, target)) { index = i; break; }
            if (index == SIZE_MAX && strcmp(target, "todas")) {
                display_begin("COMANDO: consultar");
                fputs("Tienda no configurada. Usa consultar TIENDA o consultar todas.\n", display_errors()); display_end();
            } else if (execute(config, cache, index) < 0) { failed = 1; break; }
        } else {
            display_begin("COMANDO: entrada inválida");
            fputs("Comando inválido. Usa ayuda.\n", display_errors()); display_end();
        }
    }
    display_begin("CIERRE DEL CONSULTOR");
    fputs(failed ? "El consultor terminó por un error; las tiendas continúan operando.\n" : "Consultor finalizado; historial conservado.\n", display_output());
    display_end(); government_cache_close(cache); free(config); free(cache);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
