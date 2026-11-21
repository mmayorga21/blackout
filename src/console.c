#include "display.h"
#include "console.h"
#include "work_queue.h"
#include "chain_journal.h"
#include "network.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_amount(const char *text, int64_t *amount)
{
    if (*text == '\0') return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || *end || value < 1 || value > INT64_MAX) return -1;
    *amount = (int64_t)value;
    return 0;
}

typedef struct {
    local_store *store;
    work_queue queue;
    int wake[2];
    int worker_result;
    network_runtime *network;
} console_runtime;

static void wake_receiver(console_runtime *runtime)
{
    /* Un error detiene la instancia; alcanzar el límite solo deshabilita ventas. */
    ssize_t n;
    do { n = write(runtime->wake[1], "x", 1); } while (n < 0 && errno == EINTR);
    if (n != 1) {
        display_perror("Notificar cierre de consola");
        exit(EXIT_FAILURE);
    }
}

static void stop_worker(console_runtime *runtime, queue_stop_reason reason)
{
    queue_stop(&runtime->queue, reason);
    wake_receiver(runtime);
}

static void *process_requests(void *argument)
{
    console_runtime *runtime = argument;
    work_request request;
    queue_stop_reason reason;
    while (queue_take(&runtime->queue, &request, &reason) == 0) {
        network_notify(runtime->network);
        if (reason == QUEUE_ERROR) {
            if (request.kind == REQUEST_SALE) {
                display_begin("COMANDO: venta %" PRId64, request.amount);
                fputs("Venta rechazada: solicitud pendiente no aceptada por fallo de la instancia.\n", display_errors());
                display_end();
            }
            continue;
        }
        switch (request.kind) {
        case REQUEST_SALE: display_begin("COMANDO: venta %" PRId64, request.amount); break;
        case REQUEST_STATUS: display_begin("COMANDO: estado"); break;
        case REQUEST_HISTORY: display_begin("COMANDO: historial"); break;
        case REQUEST_HELP: display_begin("COMANDO: ayuda"); break;
        case REQUEST_REMOTE: display_begin("AVISO DE RED: venta recibida"); break;
        case REQUEST_PROGRESS: break;
        }
        switch (request.kind) {
        case REQUEST_SALE: {
            int result = store_sell(runtime->store, request.amount);
            if (result < 0) {
                runtime->worker_result = -1;
                stop_worker(runtime, QUEUE_ERROR);
            } else if (result == 0) {
                if (runtime->network) {
                    sale_record sale;
                    if (chain_record(&runtime->store->journal, (size_t)runtime->store->journal.count - 1, &sale) < 0 ||
                        network_publish(runtime->network, &sale, 1) < 0) {
                        display_perror("Preparar entrega de venta durable");
                        runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
                    } else fprintf(display_output(), "Venta aceptada: %s | Importe: %" PRId64 " centavos\n", sale.id, sale.amount);
                }
                store_status(runtime->store);
                if (store_at_limit(runtime->store))
                    fprintf(display_output(), "%s\n", "Límite alcanzado: ventas deshabilitadas. Puedes usar estado, historial, ayuda o salir.");
            }
            break;
        }
        case REQUEST_STATUS:
            store_status(runtime->store);
            network_status(runtime->network);
            break;
        case REQUEST_HISTORY:
            if (journal_history(&runtime->store->journal) < 0) {
                fputs("No se pudo leer un historial íntegro; se detiene la instancia.\n", display_errors());
                runtime->worker_result = -1;
                stop_worker(runtime, QUEUE_ERROR);
            }
            break;
        case REQUEST_HELP:
            fprintf(display_output(), "%s\n", "venta CENTAVOS | estado | historial | ayuda | salir");
            break;
        case REQUEST_REMOTE: {
            sale_record sale;
            if (!runtime->network || sale_decode(request.sale_csv, &sale) < 0) {
                runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR); break;
            }
            int was_at_limit = store_at_limit(runtime->store);
            int result = network_receive(runtime->network, &request, &runtime->store->journal);
            if (result < 0) {
                display_perror("Guardar venta remota");
                runtime->store->storage_failed = 1;
                runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
                break;
            }
            if (result == 0) {
                fprintf(display_output(), "Venta remota registrada: %s\n", sale.id);
                store_status(runtime->store);
                if (!was_at_limit && store_at_limit(runtime->store))
                    fprintf(display_output(), "%s\n", "Límite alcanzado: ventas deshabilitadas. Puedes usar estado, historial, ayuda o salir.");
            } else if (result == 2) fputs("Venta remota rechazada: ID en conflicto, origen o importe inválidos.\n", display_errors());
            network_reply(runtime->network, &request, sale.id, result);
            break;
        }
        case REQUEST_PROGRESS: {
            if (network_advance(runtime->network, &request) < 0) {
                display_perror("Guardar avance de recorrido");
                runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
            }
            break;
        }
        }
        display_end();
    }
    reason = queue_reason(&runtime->queue);
    if (reason == QUEUE_ERROR) runtime->worker_result = -1;
    return NULL;
}

/* 0: continuar; 1: salir manual; 2: recepción detenida por el consumidor. */
static int submit_line(console_runtime *runtime, char *line)
{
    char *command = line;
    while (isspace((unsigned char)*command)) command++;
    size_t length = strlen(command);
    while (length && isspace((unsigned char)command[length - 1])) command[--length] = '\0';
    if (!*command) return 0;
    if (strcmp(command, "salir") == 0) return 1;
    work_request request = {0};
    if (strcmp(command, "estado") == 0) request.kind = REQUEST_STATUS;
    else if (strcmp(command, "historial") == 0) request.kind = REQUEST_HISTORY;
    else if (strcmp(command, "ayuda") == 0) request.kind = REQUEST_HELP;
    else if (strncmp(command, "venta", 5) == 0 && isspace((unsigned char)command[5])) {
        char *text = command + 5;
        while (isspace((unsigned char)*text)) text++;
        if (parse_amount(text, &request.amount) < 0) {
            display_begin("COMANDO: %s", command);
            fputs("Importe inválido: usa centavos enteros positivos dentro del rango de 64 bits.\n", display_errors());
            display_end();
            return 0;
        }
        request.kind = REQUEST_SALE;
    } else {
        display_begin("COMANDO: %s", command);
        fputs("Comando inválido. Usa ayuda.\n", display_errors());
        display_end();
        return 0;
    }
    if (queue_put(&runtime->queue, request) != 0) {
        if (request.kind == REQUEST_SALE) {
            display_begin("COMANDO: venta %" PRId64, request.amount);
            fputs("Venta rechazada: recepción detenida; la solicitud no fue aceptada.\n", display_errors());
            display_end();
        }
        return 2;
    }
    return 0;
}

static void invalid_line(void)
{
    display_begin("COMANDO: entrada inválida");
    fputs("Comando demasiado largo o con bytes inválidos.\n", display_errors());
    display_end();
}

/* -1: fallo; 0: EOF; 1: salir; 2: notificación de cierre. */
static int receive_commands(console_runtime *runtime)
{
    struct pollfd watched[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = runtime->wake[0], .events = POLLIN }
    };
    char line[256], chunk[4096];
    size_t used = 0;
    int invalid = 0;
    int interactive = isatty(STDIN_FILENO);
    if (interactive) display_prompt();
    for (;;) {
        int ready;
        do { ready = poll(watched, 2, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) { display_perror("Esperar entrada"); return -1; }
        if (watched[1].revents & (POLLIN | POLLHUP)) return 2;
        if (watched[0].revents & (POLLERR | POLLNVAL) || watched[1].revents & (POLLERR | POLLNVAL)) {
            fputs("Error en los descriptores de recepción.\n", display_errors());
            return -1;
        }
        if (!(watched[0].revents & (POLLIN | POLLHUP))) continue;
        ssize_t n;
        do { n = read(STDIN_FILENO, chunk, sizeof chunk); } while (n < 0 && errno == EINTR);
        if (n < 0) { display_perror("Leer comandos"); return -1; }
        if (n == 0) {
            if (invalid) invalid_line();
            else if (used) {
                line[used] = '\0';
                int result = submit_line(runtime, line);
                if (result != 0) return result;
            }
            return 0;
        }
        for (size_t i = 0; i < (size_t)n; i++) {
            if (chunk[i] == '\n') {
                int result = 0;
                if (invalid) invalid_line();
                else {
                    line[used] = '\0';
                    result = submit_line(runtime, line);
                }
                used = 0;
                invalid = 0;
                if (result != 0) return result;
                if (interactive) display_prompt();
            } else if (chunk[i] == '\0' || used == sizeof line - 1) invalid = 1;
            else if (!invalid) line[used++] = chunk[i];
        }
    }
}

int console_run(local_store *store)
{
    display_begin("ESTADO INICIAL");
    fprintf(display_output(), "%s\n", "Comandos: venta CENTAVOS | estado | historial | ayuda | salir");
    store_status(store);
    if (store_at_limit(store)) {
        fprintf(display_output(), "%s\n", "Límite alcanzado: ventas deshabilitadas. Puedes usar estado, historial, ayuda o salir.");
    }
    display_end();
    console_runtime runtime = { .store = store };
    if (queue_init(&runtime.queue, store->config.queue_capacity) < 0) { display_perror("Crear cola"); return -1; }
    if (pipe(runtime.wake) < 0) { display_perror("Crear notificación"); queue_destroy(&runtime.queue); return -1; }
    if (fcntl(runtime.wake[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(runtime.wake[1], F_SETFD, FD_CLOEXEC) < 0) {
        display_perror("Configurar notificación");
        close(runtime.wake[0]); close(runtime.wake[1]); queue_destroy(&runtime.queue); return -1;
    }
    pthread_t worker;
    if (store->journal.chain) {
        runtime.network = network_start(&store->config, &store->journal, &runtime.queue, runtime.wake[1]);
        if (!runtime.network) {
            close(runtime.wake[0]); close(runtime.wake[1]); queue_destroy(&runtime.queue); return -1;
        }
    }
    int result = pthread_create(&worker, NULL, process_requests, &runtime);
    if (result != 0) {
        fprintf(display_errors(), "Crear hilo de procesamiento: %s\n", strerror(result));
        network_destroy(runtime.network);
        close(runtime.wake[0]); close(runtime.wake[1]); queue_destroy(&runtime.queue); return -1;
    }
    int reception = receive_commands(&runtime);
    if (reception < 0) queue_stop(&runtime.queue, QUEUE_ERROR);
    network_quiesce(runtime.network);
    queue_finish(&runtime.queue);
    result = pthread_join(worker, NULL);
    if (result != 0) {
        fprintf(display_errors(), "Esperar al trabajador: %s\n", strerror(result));
        /* No cerrar el historial ni destruir la cola si el hilo pudiera seguir usándolos. */
        exit(EXIT_FAILURE);
    }
    queue_stop_reason reason = queue_reason(&runtime.queue);
    int failed = runtime.worker_result < 0 || reception < 0;
    if (reason == QUEUE_RUNNING) {
        display_begin(reception == 1 ? "COMANDO: salir" : "CIERRE");
        fprintf(display_output(), "%s\n", reception == 1 ? "Salida ordenada; historial conservado." : "Fin de entrada: salida ordenada.");
        display_end();
    }
    if (close(runtime.wake[0]) < 0) { display_perror("Cerrar notificación"); failed = 1; }
    if (close(runtime.wake[1]) < 0) { display_perror("Cerrar notificación"); failed = 1; }
    network_destroy(runtime.network);
    queue_destroy(&runtime.queue);
    return failed ? -1 : 0;
}
