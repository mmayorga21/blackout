#ifndef BLACKOUT_REPORT_H
#define BLACKOUT_REPORT_H
#include <stdint.h>
#include <stddef.h>

enum { REPORT_LIST_SIZE = 1100, REPORT_WIRE_SIZE = 4096 };
enum { REPORT_OPEN, REPORT_READONLY, REPORT_STORAGE_ERROR };
enum { REPORT_INDEPENDENT, REPORT_NETWORK_ACTIVE, REPORT_ISOLATED };
enum { REPORT_NOT_APPLICABLE, REPORT_NOT_CLOSING, REPORT_SYNCHRONIZING, REPORT_SYNCHRONIZED };
typedef struct {
    char store_id[33], chain_id[33];
    uint64_t count;
    int64_t own_cent, known_cent, limit_cent;
    int commercial, link, closing;
    char group[REPORT_LIST_SIZE], pending[REPORT_LIST_SIZE];
} store_report;
int report_encode(const store_report *report, char *wire, size_t capacity);
int report_decode(const char *wire, store_report *report);
void report_print(const store_report *report);
#endif
