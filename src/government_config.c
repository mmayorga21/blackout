#include "government.h"
#include "sale.h"
#include "display.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) text++;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
    return text;
}
static int number(const char *text, uintmax_t maximum, uintmax_t *value)
{
    if (!*text) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end;
    uintmax_t n = strtoumax(text, &end, 10);
    if (errno || *end || !n || n > maximum) return -1;
    *value = n; return 0;
}
int government_config_load(const char *path, government_config *config)
{
    *config = (government_config){0};
    FILE *file = fopen(path, "r"); if (!file) { display_perror("Abrir configuración de gobierno"); return -1; }
    unsigned int seen = 0; int failed = 0;
    char line[1024];
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n') && !feof(file)) { failed = 1; break; }
        char *key = trim(line); if (!*key || *key == '#') continue;
        char *value = strchr(key, '='); if (!value) { failed = 1; break; }
        *value++ = '\0'; key = trim(key); value = trim(value);
        unsigned int bit = !strcmp(key, "stores_file") ? 1U : !strcmp(key, "data_dir") ? 2U : !strcmp(key, "timeout_ms") ? 4U : 0U;
        if (!bit || (seen & bit) || !*value) { failed = 1; break; }
        seen |= bit;
        if (bit == 4) {
            uintmax_t n;
            if (number(value, 3600000, &n) < 0) { failed = 1; break; }
            config->timeout_ms = (unsigned int)n;
        } else {
            if (strlen(value) >= 256) { failed = 1; break; }
            strcpy(bit == 1 ? config->stores_file : config->data_dir, value);
        }
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed || seen != 7) { fputs("Configuración de gobierno inválida: stores_file, data_dir y timeout_ms son obligatorios y únicos.\n", display_errors()); return -1; }
    file = fopen(config->stores_file, "r"); if (!file) { display_perror("Abrir catálogo de consultas"); return -1; }
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n') && !feof(file)) { failed = 1; break; }
        char *start = trim(line); if (!*start || *start == '#') continue;
        government_endpoint endpoint; char port[16], extra;
        struct in_addr address; uintmax_t n;
        if (config->count == GOVERNMENT_STORES || sscanf(start, "%32s %32s %15s %15s %c", endpoint.id, endpoint.chain, endpoint.address, port, &extra) != 4 ||
            !sale_identifier(endpoint.id) || !strcmp(endpoint.id, "todas") || !sale_identifier(endpoint.chain) ||
            inet_pton(AF_INET, endpoint.address, &address) != 1 || number(port, 65535, &n) < 0) { failed = 1; break; }
        endpoint.port = (unsigned int)n;
        for (size_t i = 0; i < config->count; i++) if (!strcmp(config->stores[i].id, endpoint.id) ||
            (!strcmp(config->stores[i].address, endpoint.address) && config->stores[i].port == endpoint.port)) failed = 1;
        if (failed) break;
        config->stores[config->count++] = endpoint;
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed || !config->count) { fputs("Catálogo de consultas inválido: ID CADENA IPv4 PUERTO, identidades y direcciones únicas (máximo 128).\n", display_errors()); return -1; }
    return 0;
}
