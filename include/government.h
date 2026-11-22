#ifndef BLACKOUT_GOVERNMENT_H
#define BLACKOUT_GOVERNMENT_H
#include "report.h"
enum { GOVERNMENT_STORES = 128 };
typedef struct {
    char id[33], chain[33], address[16];
    unsigned int port;
} government_endpoint;
typedef struct {
    char stores_file[256], data_dir[256];
    unsigned int timeout_ms;
    size_t count;
    government_endpoint stores[GOVERNMENT_STORES];
} government_config;
typedef struct { int present; int64_t observed_at; char received[32]; store_report report; } government_observation;
typedef struct {
    int fd, lock_fd;
    government_observation latest[GOVERNMENT_STORES];
} government_cache;
int government_config_load(const char *path, government_config *config);
int government_cache_open(government_cache *cache, const government_config *config);
int government_cache_save(government_cache *cache, const government_config *config, size_t index,
                          const government_observation *observation);
void government_cache_close(government_cache *cache);
int government_run(const char *path);
#endif
