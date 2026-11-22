#include "config.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *text)
{
    while (isspace((unsigned char)*text)) text++;
    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) text[--len] = '\0';
    return text;
}

static int copy_text(char *dest, size_t capacity, const char *value)
{
    size_t len = strlen(value);
    if (len == 0 || len >= capacity) return -1;
    memcpy(dest, value, len + 1);
    return 0;
}

static int identifier(const char *value)
{
    if (*value == '\0') return -1;
    for (; *value; value++) {
        unsigned char c = (unsigned char)*value;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return -1;
    }
    return 0;
}

static int number(const char *value, int64_t minimum, int64_t maximum, int64_t *result)
{
    if (*value == '\0') return -1;
    for (const char *p = value; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
    }
    char *end = NULL;
    errno = 0;
    intmax_t parsed = strtoimax(value, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) return -1;
    *result = (int64_t)parsed;
    return 0;
}

int config_load(const char *path, store_config *config, FILE *errors)
{
    static const char *keys[] = {
        "store_id", "chain_id", "bind_address", "port", "limit_cent",
        "timeout_ms", "queue_capacity", "data_dir", "peers_file", "sale_share_delay_ms", "query_port", "control_timeout_ms", "close_timeout_ms"
    };
    store_config candidate = { .sale_share_delay_ms = 5000 };
    unsigned int seen = 0;
    unsigned int line_number = 0;
    int failed = 0;
    char line[1024];
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(errors, "No se puede leer %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, file) != NULL) {
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            fprintf(errors, "%s:%u: línea demasiado larga.\n", path, line_number);
            failed = 1;
            break;
        }
        char *key = trim(line);
        if (*key == '\0' || *key == '#') continue;
        char *equals = strchr(key, '=');
        if (equals == NULL) {
            fprintf(errors, "%s:%u: se esperaba clave=valor.\n", path, line_number);
            failed = 1;
            continue;
        }
        *equals = '\0';
        key = trim(key);
        char *value = trim(equals + 1);
        size_t index;
        for (index = 0; index < sizeof keys / sizeof keys[0]; index++) {
            if (strcmp(key, keys[index]) == 0) break;
        }
        if (index == sizeof keys / sizeof keys[0]) {
            fprintf(errors, "%s:%u: clave desconocida: %s.\n", path, line_number, key);
            failed = 1;
            continue;
        }
        unsigned int bit = 1U << index;
        if ((seen & bit) != 0) {
            fprintf(errors, "%s:%u: clave repetida: %s.\n", path, line_number, key);
            failed = 1;
            continue;
        }
        seen |= bit;
        int result = -1;
        int64_t numeric = 0;
        struct in_addr address;
        switch (index) {
        case 0:
            if (identifier(value) == 0)
                result = copy_text(candidate.store_id, sizeof candidate.store_id, value);
            break;
        case 1:
            if (identifier(value) == 0)
                result = copy_text(candidate.chain_id, sizeof candidate.chain_id, value);
            break;
        case 2:
            if (inet_pton(AF_INET, value, &address) == 1)
                result = copy_text(candidate.bind_address, sizeof candidate.bind_address, value);
            break;
        case 3:
            result = number(value, 1, 65535, &numeric);
            if (result == 0) candidate.port = (unsigned int)numeric;
            break;
        case 4:
            result = number(value, 1, INT64_MAX, &candidate.limit_cent);
            break;
        case 5:
            result = number(value, 1, 3600000, &numeric);
            if (result == 0) candidate.timeout_ms = (unsigned int)numeric;
            break;
        case 6:
            result = number(value, 1, 1000000, &numeric);
            if (result == 0) candidate.queue_capacity = (unsigned int)numeric;
            break;
        case 7:
            result = copy_text(candidate.data_dir, sizeof candidate.data_dir, value);
            break;
        case 8:
            result = copy_text(candidate.peers_file, sizeof candidate.peers_file, value);
            break;
        case 11:
        case 12:
            result = number(value, 1, CONFIG_WAIT_MAX_MS, &numeric);
            if (result == 0) {
                if (index == 11) candidate.control_timeout_ms = (unsigned int)numeric;
                else candidate.close_timeout_ms = (unsigned int)numeric;
            }
            break;
        case 10:
            result = number(value, 0, 65535, &numeric);
            if (result == 0) candidate.query_port = (unsigned int)numeric;
            break;
        case 9:
            result = number(value, 0, 60000, &numeric);
            if (result == 0) candidate.sale_share_delay_ms = (unsigned int)numeric;
            break;
        }
        if (result != 0) {
            fprintf(errors, "%s:%u: valor inválido para %s.\n", path, line_number, key);
            failed = 1;
        }
    }
    if (ferror(file)) {
        fprintf(errors, "Error al leer %s.\n", path);
        failed = 1;
    }
    if (fclose(file) != 0) {
        fprintf(errors, "Error al cerrar %s.\n", path);
        failed = 1;
    }
    const unsigned int required = (1U << 9) - 1U;
    if ((seen & required) != required) {
        fprintf(errors, "%s: faltan claves obligatorias.\n", path);
        failed = 1;
    }
    if (candidate.query_port && candidate.query_port == candidate.port) {
        fprintf(errors, "%s: query_port debe ser distinto de port.\n", path);
        failed = 1;
    }
    if ((strcmp(candidate.chain_id, "none") == 0) != (strcmp(candidate.peers_file, "none") == 0)) {
        fprintf(errors, "Configuración incoherente (%s): cadena y catálogo deben estar definidos juntos.\n", path);
        failed = 1;
    }
    if (failed) return -1;
    uint64_t control = (uint64_t)candidate.timeout_ms * 2;
    uint64_t closing = control + candidate.sale_share_delay_ms;
    if (!(seen & (1U << 11)))
        candidate.control_timeout_ms = (unsigned int)(control > CONFIG_WAIT_MAX_MS ? CONFIG_WAIT_MAX_MS : control);
    if (!(seen & (1U << 12)))
        candidate.close_timeout_ms = (unsigned int)(closing > CONFIG_WAIT_MAX_MS ? CONFIG_WAIT_MAX_MS : closing);
    *config = candidate;
    return 0;
}
