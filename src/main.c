#include "display.h"
#include "config.h"
#include "console.h"
#include "government.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    /* Configurar antes de la primera operación sobre stdout. */
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) return EXIT_FAILURE;
    /* La validación usa fgets; luego poll/read reciben comandos sin datos prefetched. */
    if (setvbuf(stdin, NULL, _IONBF, 0) != 0) return EXIT_FAILURE;
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("Uso: blackout --config RUTA [--historial]\n     blackout --gobierno RUTA\nTiendas independientes, cadenas y consultor del gobierno.");
        return EXIT_SUCCESS;
    }
    if (argc == 3 && !strcmp(argv[1], "--gobierno")) return government_run(argv[2]);
    if ((argc != 3 && argc != 4) || strcmp(argv[1], "--config") != 0 ||
        (argc == 4 && strcmp(argv[3], "--historial") != 0)) {
        fputs("Uso: blackout --config RUTA [--historial], --gobierno RUTA o --help.\n", stderr);
        return EXIT_FAILURE;
    }
    store_config config;
    while (config_load(argv[2], &config, stderr) != 0) {
        char reply[32];
        fputs("Corrige el archivo y pulsa Enter para reintentar; q para salir: ", stderr);
        if (fgets(reply, sizeof reply, stdin) == NULL || reply[0] == 'q' || reply[0] == 'Q')
            return EXIT_FAILURE;
        if (strchr(reply, '\n') == NULL) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) { }
        }
    }
    display_begin("INICIO");
    fprintf(display_output(), "Configuración válida: tienda %s, cadena %s, dirección %s:%u.\n",
           config.store_id, config.chain_id, config.bind_address, config.port);
    fprintf(display_output(), "Límite: %" PRId64 " centavos; datos: %s.\n", config.limit_cent, config.data_dir);
    local_store store;
    int opened = store_open(&store, &config);
    display_end();
    if (opened < 0) return EXIT_FAILURE;
    int result;
    if (argc == 4) {
        display_begin("CONSULTA: historial");
        result = journal_history(&store.journal);
        if (result < 0) fputs("No se pudo leer el historial.\n", display_errors());
        display_end();
    } else result = console_run(&store);
    if (store_close(&store) < 0) result = -1;
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
