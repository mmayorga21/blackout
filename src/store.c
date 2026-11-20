#include "store.h"

#include <inttypes.h>
#include <string.h>

int store_open(local_store *store, const store_config *config)
{
    memset(store, 0, sizeof *store);
    store->config = *config;
    if (strcmp(config->chain_id, "none") != 0 || strcmp(config->peers_file, "none") != 0) {
        fputs("Esta etapa solo admite tiendas independientes: chain_id=none y peers_file=none.\n", stderr);
        return -1;
    }
    return journal_open(&store->journal, config);
}

int store_at_limit(const local_store *store)
{
    return store->journal.total_cent >= store->config.limit_cent;
}

void store_status(const local_store *store)
{
    printf("Estado: %s | Tienda: %s | Ventas: %" PRIu64 " | Ingresos: %" PRId64
           " centavos | Límite: %" PRId64 " centavos\n",
           store->storage_failed ? "ERROR_ALMACENAMIENTO" : store_at_limit(store) ? "CERRADA" : "ABIERTA",
           store->config.store_id, store->journal.count, store->journal.total_cent, store->config.limit_cent);
}

int store_sell(local_store *store, int64_t amount)
{
    if (store->storage_failed || store_at_limit(store) || amount <= 0 ||
        amount > INT64_MAX - store->journal.total_cent || store->journal.count == UINT64_MAX) {
        fputs("Venta rechazada: tienda cerrada, importe inválido o desbordamiento.\n", stderr);
        return 1;
    }
    if (journal_append(&store->journal, amount) < 0) {
        perror("No se pudo confirmar la venta");
        fputs("Se detiene la instancia. El resultado puede ser incierto; debe recuperarse el historial antes de reintentar.\n", stderr);
        store->storage_failed = 1;
        return -1;
    }
    printf("Venta aceptada: %s:%" PRIu64 " | Importe: %" PRId64 " centavos\n",
           store->config.store_id, store->journal.count, amount);
    return 0;
}

int store_close(local_store *store)
{
    if (journal_close(&store->journal) < 0) { perror("Cerrar historial"); return -1; }
    return 0;
}
