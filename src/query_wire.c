#include "query_wire.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>

int64_t query_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
int query_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? -1 : 0;
}
size_t query_frame(unsigned char *wire, const char *body, size_t length)
{
    memcpy(wire, "BQ09", 4);
    uint32_t n = htonl((uint32_t)length); memcpy(wire + 4, &n, 4);
    memcpy(wire + QUERY_HEADER, body, length); return QUERY_HEADER + length;
}
int query_length(const unsigned char *header, size_t maximum, size_t *length)
{
    if (memcmp(header, "BQ09", 4)) return -1;
    uint32_t n; memcpy(&n, header + 4, 4); n = ntohl(n);
    if (!n || n > maximum) return -1;
    *length = (size_t)n; return 0;
}
void query_request(unsigned char *wire) { (void)query_frame(wire, "CONSULTAR\n", 10); }
