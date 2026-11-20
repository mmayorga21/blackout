#ifndef BLACKOUT_SALE_H
#define BLACKOUT_SALE_H

#include <stdint.h>
#include <stddef.h>

enum { SALE_ID_SIZE = 96, SALE_CSV_SIZE = 256 };
typedef struct {
    char id[SALE_ID_SIZE];
    char origin[33];
    char stamp[32];
    uint64_t sequence;
    int64_t amount;
} sale_record;

int sale_encode(const sale_record *sale, char *line, size_t capacity);
int sale_decode(const char *line, sale_record *sale);
int sale_timestamp(char *stamp, size_t capacity);
uint32_t sale_crc(const void *data, size_t length);
int sale_identifier(const char *text);

#endif
