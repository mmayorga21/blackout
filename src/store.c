#include "display.h"
#include "store.h"
#include "chain_journal.h"

#include <inttypes.h>
#include <string.h>

int store_open(local_store *store, const store_config *config)
{
    memset(store, 0, sizeof *store);
    store->config = *config;
    if ((strcmp(config->chain_id, "none") == 0) != (strcmp(config->peers_file, "none") == 0)) {
        fputs("Configuración incoherente: cadena y catálogo deben estar definidos juntos.\n", display_errors());
        return -1;
    }
    return journal_open(&store->journal, config);
}

static int64_t own_income(const local_store *store)
{
    return store->journal.chain ? store->journal.own_total_cent : store->journal.total_cent;
}

int store_at_limit(const local_store *store)
{
    return own_income(store) >= store->config.limit_cent;
}

void store_status(const local_store *store)
{
    fprintf(display_output(), "Estado: %s | Tienda: %s | Ventas: %" PRIu64 "\nIngresos: %" PRId64
           " centavos | Límite: %" PRId64 " centavos\n",
           store->storage_failed ? "ERROR_ALMACENAMIENTO" : store_at_limit(store) ? "SOLO_CONSULTA" : "ABIERTA",
           store->config.store_id, store->journal.count, own_income(store), store->config.limit_cent);
    if (store->journal.chain)
        fprintf(display_output(), "Cadena: %s | Ingresos propios: %" PRId64 " | Ingresos conocidos de cadena: %" PRId64 " centavos\n",
               store->config.chain_id, store->journal.own_total_cent, store->journal.total_cent);
}

int store_sell(local_store *store, int64_t amount)
{
    if (store->storage_failed || store_at_limit(store) || amount <= 0 ||
        amount > INT64_MAX - store->journal.total_cent || store->journal.count == UINT64_MAX ||
        (store->journal.chain && store->journal.own_sequence == UINT64_MAX)) {
        fputs("Venta rechazada: ventas deshabilitadas, importe inválido o desbordamiento.\n", display_errors());
        return 1;
    }
    if (journal_append(&store->journal, amount) < 0) {
        display_perror("No se pudo confirmar la venta");
        fputs("Se detiene la instancia. El resultado puede ser incierto; debe recuperarse el historial antes de reintentar.\n", display_errors());
        store->storage_failed = 1;
        return -1;
    }
    if (!store->journal.chain) fprintf(display_output(), "Venta aceptada: %s:%" PRIu64 " | Importe: %" PRId64 " centavos\n",
                  store->config.store_id, store->journal.count, amount);
    return 0;
}

int store_close(local_store *store)
{
    if (journal_close(&store->journal) < 0) { display_perror("Cerrar historial"); return -1; }
    return 0;
}
