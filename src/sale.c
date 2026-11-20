#include "sale.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

uint32_t sale_crc(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        crc ^= bytes[i];
        for (unsigned int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
    }
    return ~crc;
}

int sale_identifier(const char *text)
{
    size_t length = strlen(text);
    if (!length || length > 32) return 0;
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static int number(const char *text, uint64_t maximum, uint64_t *value)
{
    if (!*text) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    uintmax_t n = strtoumax(text, &end, 10);
    if (errno || *end || n > maximum) return -1;
    *value = (uint64_t)n;
    return 0;
}

static int valid_stamp(const char *text)
{
    if (strlen(text) != 24) return 0;
    for (size_t i = 0; i < 24; i++) {
        if (i == 4 || i == 7) { if (text[i] != '-') return 0; }
        else if (i == 10) { if (text[i] != 'T') return 0; }
        else if (i == 13 || i == 16) { if (text[i] != ':') return 0; }
        else if (i == 19) { if (text[i] != '+' && text[i] != '-') return 0; }
        else if (text[i] < '0' || text[i] > '9') return 0;
    }
    unsigned int month, day, hour, minute, second, zh, zm;
    if (sscanf(text + 5, "%2u-%2uT%2u:%2u:%2u", &month, &day, &hour, &minute, &second) != 5 ||
        sscanf(text + 20, "%2u%2u", &zh, &zm) != 2) return 0;
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 &&
           second <= 60 && zh <= 23 && zm <= 59;
}

int sale_timestamp(char *stamp, size_t capacity)
{
    time_t now = time(NULL);
    struct tm local;
    if (now == (time_t)-1 || localtime_r(&now, &local) == NULL ||
        !strftime(stamp, capacity, "%Y-%m-%dT%H:%M:%S%z", &local) || !valid_stamp(stamp)) {
        errno = EINVAL; return -1;
    }
    return 0;
}

int sale_encode(const sale_record *sale, char *line, size_t capacity)
{
    int n = snprintf(line, capacity, "%s,%s,%" PRId64 ".%02" PRId64,
                     sale->id, sale->stamp, sale->amount / 100, sale->amount % 100);
    if (n < 0 || (size_t)n >= capacity) { errno = EOVERFLOW; return -1; }
    uint32_t crc = sale_crc(line, (size_t)n);
    int tail = snprintf(line + n, capacity - (size_t)n, ",%08" PRIx32 "\n", crc);
    if (tail < 0 || (size_t)tail >= capacity - (size_t)n) { errno = EOVERFLOW; return -1; }
    return n + tail;
}

int sale_decode(const char *line, sale_record *sale)
{
    size_t length = strlen(line);
    if (!length || length >= SALE_CSV_SIZE || line[length - 1] != '\n') return -1;
    char copy[SALE_CSV_SIZE];
    memcpy(copy, line, length + 1);
    copy[length - 1] = '\0';
    char *checksum = strrchr(copy, ',');
    if (!checksum || strlen(checksum + 1) != 8) return -1;
    char expected[9];
    snprintf(expected, sizeof expected, "%08" PRIx32, sale_crc(copy, (size_t)(checksum - copy)));
    if (strcmp(expected, checksum + 1)) return -1;
    *checksum = '\0';
    char *stamp = strchr(copy, ',');
    if (!stamp) return -1;
    *stamp++ = '\0';
    char *value = strchr(stamp, ',');
    if (!value) return -1;
    *value++ = '\0';
    if (strlen(copy) >= sizeof sale->id || !valid_stamp(stamp)) return -1;
    *sale = (sale_record){0};
    strcpy(sale->id, copy);
    strcpy(sale->stamp, stamp);
    char *period = strchr(copy, ':');
    if (!period) return -1;
    *period++ = '\0';
    if (!sale_identifier(copy)) return -1;
    strcpy(sale->origin, copy);
    char *sequence = strchr(period, ':');
    if (!sequence) return -1;
    *sequence++ = '\0';
    if (strlen(period) != 32) return -1;
    for (size_t i = 0; i < 32; i++)
        if (!((period[i] >= '0' && period[i] <= '9') || (period[i] >= 'a' && period[i] <= 'f'))) return -1;
    if (number(sequence, UINT64_MAX, &sale->sequence) < 0 || !sale->sequence) return -1;
    char *point = strchr(value, '.');
    if (!point || strlen(point + 1) != 2) return -1;
    *point++ = '\0';
    uint64_t whole, fraction;
    if (number(value, INT64_MAX / 100, &whole) < 0 || number(point, 99, &fraction) < 0 ||
        whole > ((uint64_t)INT64_MAX - fraction) / 100) return -1;
    sale->amount = (int64_t)(whole * 100 + fraction);
    if (!sale->amount) return -1;
    /* Aceptar solo la representación canónica evita IDs equivalentes con ceros iniciales. */
    char canonical[SALE_CSV_SIZE];
    if (sale_encode(sale, canonical, sizeof canonical) < 0 || strcmp(line, canonical)) return -1;
    char number_text[32];
    snprintf(number_text, sizeof number_text, "%" PRIu64, sale->sequence);
    if (strcmp(number_text, sequence)) return -1;
    return 0;
}
