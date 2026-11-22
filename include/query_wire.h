#ifndef BLACKOUT_QUERY_WIRE_H
#define BLACKOUT_QUERY_WIRE_H
#include <stdint.h>
#include <stddef.h>
enum { QUERY_HEADER = 8, QUERY_REQUEST_SIZE = 18 };
int64_t query_milliseconds(void);
int query_nonblocking(int fd);
size_t query_frame(unsigned char *wire, const char *body, size_t length);
int query_length(const unsigned char *header, size_t maximum, size_t *length);
void query_request(unsigned char *wire);
#endif
