#ifndef BLACKOUT_GENERATOR_H
#define BLACKOUT_GENERATOR_H

#include <stdint.h>
#include <sys/types.h>

typedef struct {
    int64_t minimum, maximum;
    unsigned int interval_ms;
    uint64_t count, seed;
    int seeded;
} generator_options;

/* Inicializar con pid=0, fd=-1. Un solo propietario: el receptor de consola
 * crea, lee y recoge al hijo. */
typedef struct {
    pid_t pid;
    int fd;
    char line[64];
    unsigned int used;
} purchase_generator;

int generator_parse(int argc, char **argv, generator_options *options);
int generator_run(const generator_options *options, pid_t parent);
int generator_start(purchase_generator *generator, const generator_options *options);
/* 1 compra; 0 aún no hay línea; 2 EOF; -1 error. No bloquea. */
int generator_read(purchase_generator *generator, int64_t *amount);
/* terminate=0 tras EOF; terminate=1 detiene primero. Siempre recoge al hijo. */
int generator_finish(purchase_generator *generator, int terminate);

#endif
