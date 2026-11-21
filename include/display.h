#ifndef BLACKOUT_DISPLAY_H
#define BLACKOUT_DISPLAY_H

#include <stdio.h>

/* Cada hilo prepara su bloque sin bloquear las tareas de otros hilos. */
void display_begin(const char *format, ...);
void display_end(void);
FILE *display_output(void);
FILE *display_errors(void);
void display_perror(const char *message);
void display_notice(const char *format, ...);
void display_prompt(void);

#endif
