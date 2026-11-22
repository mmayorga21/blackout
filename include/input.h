#ifndef BLACKOUT_INPUT_H
#define BLACKOUT_INPUT_H
#include <stddef.h>
#include <stdio.h>
/* 0 EOF; 1 línea; 2 línea demasiado larga/con NUL; -1 error de lectura. */
int input_line(FILE *stream, char *line, size_t capacity);
/* Enter reintenta el mismo archivo; q/EOF cancela. Antes de iniciar hilos. */
int input_retry_configuration(void);
/* Divide por espacios reales; rechaza campos de más o de menos. Modifica text. */
int input_fields(char *text, char **fields, size_t expected);
#endif
