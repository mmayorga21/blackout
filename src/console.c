#include "display.h"
#include "console.h"
#include "work_queue.h"
#include "chain_journal.h"
#include "network.h"
#include "query.h"
#include "generator.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

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
    query_runtime *query;
    /* El trabajador publica permiso; el receptor es dueño exclusivo del hijo. */
    atomic_int sales_enabled, generator_waiting;
    purchase_generator generator;
    int64_t generated_amount;
    int generated_pending;
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

static void publish_sales_state(console_runtime *runtime)
{
    int enabled = !runtime->store->storage_failed && !store_at_limit(runtime->store);
    if (atomic_exchange(&runtime->sales_enabled, enabled) && !enabled) wake_receiver(runtime);
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
        if (atomic_exchange(&runtime->generator_waiting, 0)) wake_receiver(runtime);
        network_notify(runtime->network);
        query_notify(runtime->query);
        if (reason == QUEUE_ERROR) {
            if (request.kind == REQUEST_SALE) {
                display_begin("COMANDO: venta %" PRId64, request.amount);
                fputs("Venta rechazada: solicitud pendiente no aceptada por fallo de la instancia.\n", display_errors());
                display_end();
            }
            continue;
        }
        if (runtime->network && network_maintain(runtime->network, runtime->store->journal.total_cent) < 0) {
            display_perror("Guardar estado de cadena");
            runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR); continue;
        }
        runtime->store->closing = network_frozen(runtime->network);
        publish_sales_state(runtime);
        switch (request.kind) {
        case REQUEST_SALE: display_begin("COMANDO: venta %" PRId64, request.amount); break;
        case REQUEST_STATUS: display_begin("COMANDO: estado"); break;
        case REQUEST_HISTORY: display_begin("COMANDO: historial"); break;
        case REQUEST_HELP: display_begin("COMANDO: ayuda"); break;
        case REQUEST_REMOTE: display_begin("AVISO DE RED: venta recibida"); break;
        case REQUEST_DISCONNECT: display_begin("COMANDO: desconectar"); break;
        case REQUEST_CONNECT: display_begin("COMANDO: conectar"); break;
        case REQUEST_CLOSE: display_begin("COMANDO: cerrar"); break;
        case REQUEST_SYNC: display_begin("AVISO DE RED: cierre de cadena"); break;
        case REQUEST_PROGRESS: case REQUEST_CHECKPOINT: case REQUEST_EXIT: case REQUEST_QUERY: break;
        }
        switch (request.kind) {
        case REQUEST_SALE: {
            runtime->store->closing = network_frozen(runtime->network);
            int result;
            if (runtime->network) {
                result = store_can_sell(runtime->store, request.amount);
                if (!result) result = network_local(runtime->network, &runtime->store->journal, request.amount);
            } else result = store_sell(runtime->store, request.amount);
            if (result < 0) {
                if (runtime->network) {
                    display_perror("No se pudo confirmar la venta");
                    fputs("Se detiene la instancia. Consulta el historial recuperado antes de reintentar la compra.\n", display_errors());
                    runtime->store->storage_failed = 1;
                }
                runtime->worker_result = -1;
                stop_worker(runtime, QUEUE_ERROR);
            } else if (result == 0) {
                if (network_maintain(runtime->network, runtime->store->journal.total_cent) < 0) {
                    display_perror("Guardar cierre de cadena");
                    runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
                }
                runtime->store->closing = network_frozen(runtime->network);
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
            fprintf(display_output(), "%s\n", "venta CENTAVOS | generar MIN MAX INTERVALO_MS CANTIDAD [SEMILLA] | detener | estado | historial | ayuda | desconectar | conectar | cerrar | salir");
            break;
        case REQUEST_REMOTE: {
            sale_record sale;
            if (!runtime->network || sale_decode(request.sale_csv, &sale) < 0) {
                runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR); break;
            }
            int was_at_limit = store_at_limit(runtime->store);
            int result = network_receive(runtime->network, &request, &runtime->store->journal);
            if (result >= 0 && network_maintain(runtime->network, runtime->store->journal.total_cent) < 0) result = -1;
            runtime->store->closing = network_frozen(runtime->network);
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
        case REQUEST_DISCONNECT:
        case REQUEST_CONNECT:
            if (network_connection(runtime->network, request.kind == REQUEST_CONNECT) < 0) {
                display_perror("Guardar conexión de cadena");
                runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
            }
            break;
        case REQUEST_CLOSE:
            if (runtime->network) {
                if (network_close_sales(runtime->network) < 0) {
                    display_perror("Guardar cierre de cadena");
                    runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
                }
                runtime->store->closing = network_frozen(runtime->network);
                fputs("Cierre de cadena solicitado. Se completarán las ventas aceptadas y la sincronización del grupo conectado.\n", display_output());
            } else fputs("La tienda independiente termina con salir.\n", display_output());
            break;
        case REQUEST_SYNC:
            fputs("Cierre de cadena recibido: nuevas ventas deshabilitadas; sincronización en curso.\n", display_output());
            break;
        case REQUEST_CHECKPOINT:
            if (network_checkpoint(runtime->network) < 0) {
                display_perror("Guardar constancia de cierre");
                runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
            }
            break;
        case REQUEST_QUERY: {
            store_report report = { .count = runtime->store->journal.count,
                .own_cent = runtime->store->journal.chain ? runtime->store->journal.own_total_cent : runtime->store->journal.total_cent,
                .known_cent = runtime->store->journal.total_cent, .limit_cent = runtime->store->config.limit_cent,
                .commercial = runtime->store->storage_failed ? REPORT_STORAGE_ERROR : store_at_limit(runtime->store) ? REPORT_READONLY : REPORT_OPEN };
            strcpy(report.store_id, runtime->store->config.store_id); strcpy(report.chain_id, runtime->store->config.chain_id);
            strcpy(report.group, "."); strcpy(report.pending, ".");
            if (network_observe(runtime->network, &report) == 0) query_reply(runtime->query, &request, &report);
            break;
        }
        case REQUEST_EXIT:
            network_prepare_exit(runtime->network);
            break;
        case REQUEST_PROGRESS: {
            if (network_advance(runtime->network, &request) < 0) {
                display_perror("Guardar avance de recorrido");
                runtime->worker_result = -1; stop_worker(runtime, QUEUE_ERROR);
            }
            break;
        }
        }
        display_end();
        runtime->store->closing = network_frozen(runtime->network);
        publish_sales_state(runtime);
    }
    reason = queue_reason(&runtime->queue);
    if (reason == QUEUE_ERROR) runtime->worker_result = -1;
    return NULL;
}

static int stop_generator(console_runtime *runtime, const char *cause)
{
    if (runtime->generator.pid <= 0) return 0;
    display_begin("GENERADOR: detenido");
    int result = generator_finish(&runtime->generator, 1);
    runtime->generated_pending = 0;
    atomic_store(&runtime->generator_waiting, 0);
    if (result < 0) display_perror("Terminar generador");
    else fprintf(display_output(), "%s Las solicitudes ya encoladas conservan las validaciones de venta.\n", cause);
    display_end(); return result;
}

static void start_generator(console_runtime *runtime, char *arguments)
{
    display_begin("COMANDO: generar");
    char *fields[5], *save = NULL;
    int count = 0;
    char *field = strtok_r(arguments, " \t\r\n\v\f", &save);
    while (field && count < 5) {
        fields[count++] = field; field = strtok_r(NULL, " \t\r\n\v\f", &save);
    }
    generator_options options;
    if (field || generator_parse(count, fields, &options) < 0)
        fputs("Uso: generar MIN MAX INTERVALO_MS CANTIDAD [SEMILLA]. Importes positivos, MIN <= MAX; intervalo 1..3600000; cantidad positiva; semilla opcional sin signo.\n", display_errors());
    else if (runtime->generator.pid > 0)
        fputs("Ya hay un generador activo. Usa detener antes de iniciar otro.\n", display_errors());
    else if (!atomic_load(&runtime->sales_enabled))
        fputs("Generador rechazado: la tienda tiene las ventas deshabilitadas.\n", display_errors());
    else if (generator_start(&runtime->generator, &options) < 0) display_perror("Iniciar generador");
    else fprintf(display_output(), "Generador iniciado: PID %jd | Importes: %" PRId64 "..%" PRId64
        " centavos | Intervalo: %u ms | Solicitudes: %" PRIu64 "\n",
        (intmax_t)runtime->generator.pid, options.minimum, options.maximum, options.interval_ms, options.count);
    display_end();
}

/* Una compra por vuelta: stdin conserva prioridad y la cola llena no bloquea
 * al receptor. La tubería aplica contrapresión al proceso productor. */
static int receive_generated(console_runtime *runtime)
{
    if (runtime->generator.pid <= 0) return 0;
    if (!atomic_load(&runtime->sales_enabled)) return stop_generator(runtime, "Ventas deshabilitadas.");
    if (!runtime->generated_pending) {
        int result = generator_read(&runtime->generator, &runtime->generated_amount);
        if (!result) return 0;
        if (result == 2) {
            display_begin("GENERADOR: finalizado");
            int ended = generator_finish(&runtime->generator, 0);
            if (ended < 0) display_perror("El generador terminó con error");
            else fputs("Se completó la emisión de solicitudes; su aceptación se informa en cada venta.\n", display_output());
            display_end(); return 0;
        }
        if (result < 0) { display_perror("Leer generador"); return stop_generator(runtime, "Error de comunicación."); }
        runtime->generated_pending = 1;
    }
    work_request request = { .kind = REQUEST_SALE, .amount = runtime->generated_amount };
    /* Publicar antes de probar espacio evita perder la notificación del consumidor. */
    atomic_store(&runtime->generator_waiting, 1);
    int result = queue_try_put(&runtime->queue, request);
    if (result != 2) {
        atomic_store(&runtime->generator_waiting, 0);
        runtime->generated_pending = 0;
    }
    return 0;
}

/* -1: error de control; 0: continuar; 1: salir manual; 2: consumidor detenido. */
static int submit_line(console_runtime *runtime, char *line)
{
    char *command = line;
    while (isspace((unsigned char)*command)) command++;
    size_t length = strlen(command);
    while (length && isspace((unsigned char)command[length - 1])) command[--length] = '\0';
    if (!*command) return 0;
    if (strcmp(command, "salir") == 0) return 1;
    if (!strcmp(command, "detener")) {
        if (runtime->generator.pid > 0) return stop_generator(runtime, "Detención manual.");
        display_begin("COMANDO: detener"); fputs("No hay un generador activo.\n", display_output()); display_end(); return 0;
    }
    if (!strcmp(command, "generar") || (!strncmp(command, "generar", 7) && isspace((unsigned char)command[7]))) {
        start_generator(runtime, command + 7); return 0;
    }
    work_request request = {0};
    if (strcmp(command, "estado") == 0) request.kind = REQUEST_STATUS;
    else if (strcmp(command, "historial") == 0) request.kind = REQUEST_HISTORY;
    else if (strcmp(command, "ayuda") == 0) request.kind = REQUEST_HELP;
    else if (strcmp(command, "desconectar") == 0) request.kind = REQUEST_DISCONNECT;
    else if (strcmp(command, "conectar") == 0) request.kind = REQUEST_CONNECT;
    else if (strcmp(command, "cerrar") == 0) request.kind = REQUEST_CLOSE;
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
    struct pollfd watched[3] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = runtime->wake[0], .events = POLLIN },
        { .fd = -1, .events = POLLIN }
    };
    char line[256], chunk[4096];
    size_t used = 0;
    int invalid = 0;
    int interactive = isatty(STDIN_FILENO);
    if (interactive) display_prompt();
    while (queue_reason(&runtime->queue) == QUEUE_RUNNING) {
        if (runtime->generator.pid > 0 && !atomic_load(&runtime->sales_enabled))
            if (stop_generator(runtime, "Ventas deshabilitadas.") < 0) return -1;
        watched[2].fd = runtime->generated_pending ? -1 : runtime->generator.fd;
        int ready;
        do { ready = poll(watched, 3, -1); } while (ready < 0 && errno == EINTR);
        if (ready < 0) { display_perror("Esperar entrada"); return -1; }
        if (watched[1].revents & (POLLIN | POLLHUP)) {
            char notifications[128]; ssize_t n;
            do { n = read(runtime->wake[0], notifications, sizeof notifications); } while (n > 0 || (n < 0 && errno == EINTR));
            if (queue_reason(&runtime->queue) != QUEUE_RUNNING) return 2;
        }
        if (watched[0].revents & (POLLERR | POLLNVAL) || watched[1].revents & (POLLERR | POLLNVAL)) {
            fputs("Error en los descriptores de recepción.\n", display_errors());
            return -1;
        }
        if (watched[2].revents & (POLLERR | POLLNVAL)) return -1;
        if (!(watched[0].revents & (POLLIN | POLLHUP))) {
            if (receive_generated(runtime) < 0) return -1;
            continue;
        }
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
        if (receive_generated(runtime) < 0) return -1;
    }
    return 2;
}

int console_run(local_store *store)
{
    console_runtime runtime = { .store = store, .generator = { .fd = -1 } };
    atomic_init(&runtime.sales_enabled, !store_at_limit(store));
    atomic_init(&runtime.generator_waiting, 0);
    if (queue_init(&runtime.queue, store->config.queue_capacity) < 0) { display_perror("Crear cola"); return -1; }
    if (pipe(runtime.wake) < 0) { display_perror("Crear notificación"); queue_destroy(&runtime.queue); return -1; }
    if (fcntl(runtime.wake[0], F_SETFD, FD_CLOEXEC) < 0 || fcntl(runtime.wake[1], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(runtime.wake[0], F_SETFL, O_NONBLOCK) < 0) {
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
    if (store->config.query_port) {
        runtime.query = query_start(&store->config, &runtime.queue);
        if (!runtime.query) {
            network_destroy(runtime.network);
            close(runtime.wake[0]); close(runtime.wake[1]); queue_destroy(&runtime.queue); return -1;
        }
    }
    store->closing = network_frozen(runtime.network);
    atomic_store(&runtime.sales_enabled, !store_at_limit(store));
    display_begin("ESTADO INICIAL");
    fprintf(display_output(), "%s\n", "Comandos: venta CENTAVOS | generar MIN MAX INTERVALO_MS CANTIDAD [SEMILLA] | detener | estado | historial | ayuda | desconectar | conectar | cerrar | salir");
    store_status(store);
    if (runtime.query) fprintf(display_output(), "Consultas de gobierno: %s:%u\n", store->config.bind_address, store->config.query_port);
    if (store_at_limit(store)) {
        fprintf(display_output(), "%s\n", "Límite alcanzado: ventas deshabilitadas. Puedes usar estado, historial, ayuda o salir.");
    }
    display_end();
    int result = pthread_create(&worker, NULL, process_requests, &runtime);
    if (result != 0) {
        fprintf(display_errors(), "Crear hilo de procesamiento: %s\n", strerror(result));
        query_destroy(runtime.query);
        network_destroy(runtime.network);
        close(runtime.wake[0]); close(runtime.wake[1]); queue_destroy(&runtime.queue); return -1;
    }
    int reception = receive_commands(&runtime);
    if (stop_generator(&runtime, "Salida de la tienda.") < 0) reception = -1;
    if (reception < 0) queue_stop(&runtime.queue, QUEUE_ERROR);
    if (runtime.network && reception >= 0 && queue_reason(&runtime.queue) == QUEUE_RUNNING) {
        work_request finish = { .kind = REQUEST_EXIT };
        if (!queue_put(&runtime.queue, finish)) network_wait_exit(runtime.network);
    }
    query_stop(runtime.query);
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
    query_destroy(runtime.query);
    network_destroy(runtime.network);
    queue_destroy(&runtime.queue);
    return failed ? -1 : 0;
}
