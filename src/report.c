#include "report.h"
#include "sale.h"
#include "display.h"
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static int identifiers(const char *text)
{
    if (!strcmp(text, ".")) return 1;
    if (!*text || strlen(text) >= REPORT_LIST_SIZE) return 0;
    char copy[REPORT_LIST_SIZE]; strcpy(copy, text);
    char *cursor = copy;
    size_t count = 0;
    char *seen[32];
    while (cursor) {
        char *next = strchr(cursor, ','); if (next) *next++ = '\0';
        if (!sale_identifier(cursor) || count == 32) return 0;
        for (size_t i = 0; i < count; i++) if (!strcmp(seen[i], cursor)) return 0;
        seen[count++] = cursor;
        cursor = next;
    }
    return 1;
}
static int integer(const char *text, uint64_t maximum, uint64_t *value)
{
    if (!*text) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0; char *end;
    uintmax_t result = strtoumax(text, &end, 10);
    if (errno || *end || result > maximum) return -1;
    *value = (uint64_t)result; return 0;
}
int report_encode(const store_report *report, char *wire, size_t capacity)
{
    int n = snprintf(wire, capacity, "Q9|%s|%s|%d|%" PRIu64 "|%" PRId64 "|%" PRId64 "|%" PRId64 "|%d|%d|%s|%s",
        report->store_id, report->chain_id, report->commercial, report->count, report->own_cent,
        report->known_cent, report->limit_cent, report->link, report->closing, report->group, report->pending);
    if (n < 0 || (size_t)n >= capacity) return -1;
    int tail = snprintf(wire + n, capacity - (size_t)n, "|%08" PRIx32 "\n", sale_crc(wire, (size_t)n));
    return tail < 0 || (size_t)tail >= capacity - (size_t)n ? -1 : n + tail;
}
int report_decode(const char *wire, store_report *report)
{
    size_t length = strlen(wire);
    if (!length || length >= REPORT_WIRE_SIZE || wire[length - 1] != '\n') return -1;
    char copy[REPORT_WIRE_SIZE]; memcpy(copy, wire, length + 1); copy[length - 1] = '\0';
    char *crc = strrchr(copy, '|'), expected[9];
    if (!crc || strlen(crc + 1) != 8) return -1;
    snprintf(expected, sizeof expected, "%08" PRIx32, sale_crc(copy, (size_t)(crc - copy)));
    if (strcmp(expected, crc + 1)) return -1;
    *crc = '\0';
    char *parts[12], *cursor = copy;
    for (size_t i = 0; i < 11; i++) {
        parts[i] = cursor;
        cursor = strchr(cursor, '|'); if (!cursor) return -1;
        *cursor++ = '\0';
    }
    parts[11] = cursor;
    if (strcmp(parts[0], "Q9") || !sale_identifier(parts[1]) || !sale_identifier(parts[2]) ||
        !identifiers(parts[10]) || !identifiers(parts[11])) return -1;
    uint64_t state, count, own, known, limit, link, closing;
    if (integer(parts[3], REPORT_STORAGE_ERROR, &state) < 0 || integer(parts[4], UINT64_MAX, &count) < 0 ||
        integer(parts[5], INT64_MAX, &own) < 0 || integer(parts[6], INT64_MAX, &known) < 0 ||
        integer(parts[7], INT64_MAX, &limit) < 0 || !limit || integer(parts[8], REPORT_ISOLATED, &link) < 0 ||
        integer(parts[9], REPORT_SYNCHRONIZED, &closing) < 0 || own > known || (!count != !known)) return -1;
    if (!strcmp(parts[2], "none")) {
        if (link != REPORT_INDEPENDENT || closing != REPORT_NOT_APPLICABLE || own != known ||
            strcmp(parts[10], ".") || strcmp(parts[11], ".")) return -1;
    } else if (link == REPORT_INDEPENDENT || closing == REPORT_NOT_APPLICABLE || !strcmp(parts[10], ".")) return -1;
    if (state == REPORT_OPEN && (known >= limit || closing >= REPORT_SYNCHRONIZING)) return -1;
    *report = (store_report){ .count = count, .own_cent = (int64_t)own, .known_cent = (int64_t)known,
        .limit_cent = (int64_t)limit, .commercial = (int)state, .link = (int)link, .closing = (int)closing };
    strcpy(report->store_id, parts[1]); strcpy(report->chain_id, parts[2]);
    strcpy(report->group, parts[10]); strcpy(report->pending, parts[11]); return 0;
}
void report_print(const store_report *report)
{
    static const char *states[] = { "ABIERTA", "SOLO_CONSULTA", "ERROR_ALMACENAMIENTO" };
    fprintf(display_output(), "Tienda: %s | Cadena: %s\nEstado comercial informado: %s\nVentas registradas: %" PRIu64
        "\nIngresos propios: %" PRId64 " centavos\n", report->store_id, report->chain_id,
        states[report->commercial], report->count, report->own_cent);
    if (strcmp(report->chain_id, "none")) {
        static const char *links[] = { "INDEPENDIENTE", "ACTIVADA", "AISLADA" };
        static const char *closing[] = { "NO_APLICA", "NO_INICIADO", "SINCRONIZANDO", "SINCRONIZADO_CONECTADAS" };
        fprintf(display_output(), "Ingresos conocidos de cadena: %" PRId64 " centavos\nLímite compartido de cadena: %" PRId64
            " centavos\nRed de cadena: %s\nCierre informado: %s\nGrupo conectado observado: %s\nRecuperación pendiente con: %s\n",
            report->known_cent, report->limit_cent, links[report->link], closing[report->closing], report->group,
            !strcmp(report->pending, ".") ? "ninguna en la vista comprobada" : report->pending);
    } else fprintf(display_output(), "Límite propio: %" PRId64 " centavos\n", report->limit_cent);
}
