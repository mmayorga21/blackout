#include "console.h"

#include <ctype.h>
#include <errno.h>
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

int console_run(local_store *store)
{
    char line[256];
    int interactive = isatty(STDIN_FILENO);
    puts("Comandos: venta CENTAVOS | estado | historial | ayuda | salir");
    store_status(store);
    while (!store_at_limit(store)) {
        if (interactive) { fputs("blackout> ", stdout); fflush(stdout); }
        if (fgets(line, sizeof line, stdin) == NULL) {
            if (ferror(stdin)) { perror("Leer comandos"); return -1; }
            puts("Fin de entrada: salida ordenada.");
            return 0;
        }
        if (strchr(line, '\n') == NULL && !feof(stdin)) {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) { }
            fputs("Comando demasiado largo.\n", stderr);
            continue;
        }
        char *command = line;
        while (isspace((unsigned char)*command)) command++;
        size_t length = strlen(command);
        while (length && isspace((unsigned char)command[length - 1])) command[--length] = '\0';
        if (!*command) continue;
        if (strcmp(command, "salir") == 0) { puts("Salida ordenada; historial conservado."); return 0; }
        if (strcmp(command, "estado") == 0) { store_status(store); continue; }
        if (strcmp(command, "historial") == 0) {
            if (journal_history(&store->journal) < 0) {
                fputs("No se pudo leer un historial íntegro; se detiene la instancia.\n", stderr);
                return -1;
            }
            continue;
        }
        if (strcmp(command, "ayuda") == 0) { puts("venta CENTAVOS | estado | historial | ayuda | salir"); continue; }
        if (strncmp(command, "venta", 5) == 0 && isspace((unsigned char)command[5])) {
            char *text = command + 5;
            while (isspace((unsigned char)*text)) text++;
            int64_t amount;
            if (parse_amount(text, &amount) < 0) {
                fputs("Importe inválido: usa centavos enteros positivos dentro del rango de 64 bits.\n", stderr);
                continue;
            }
            int result = store_sell(store, amount);
            if (result < 0) return -1;
            if (result == 0) store_status(store);
        } else fputs("Comando inválido. Usa ayuda.\n", stderr);
    }
    puts("Límite alcanzado: tienda cerrada; no se aceptan compras nuevas.");
    return 0;
}
