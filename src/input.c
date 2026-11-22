#include "input.h"
#include <ctype.h>
#include <errno.h>
#include <string.h>

int input_line(FILE *stream, char *line, size_t capacity)
{
    if (capacity < 2) { errno = EINVAL; return -1; }
    size_t used = 0;
    int invalid = 0, c = fgetc(stream);
    while (c != EOF && c != '\n') {
        if (!c || used == capacity - 1) invalid = 1;
        else if (!invalid) line[used++] = (char)c;
        c = fgetc(stream);
    }
    line[used] = '\0';
    if (c == EOF && ferror(stream)) return -1;
    if (invalid) return 2;
    return c == EOF && !used ? 0 : 1;
}
int input_retry_configuration(void)
{
    int choice = -1;
    while (choice < 0) {
        fputs("Corrige el archivo y pulsa Enter para reintentar; q para salir: ", stderr);
        fflush(stderr);
        char reply[32]; int result = input_line(stdin, reply, sizeof reply);
        if (result <= 0) return 0;
        char *value = reply;
        while (isspace((unsigned char)*value)) value++;
        size_t length = strlen(value);
        while (length && isspace((unsigned char)value[length - 1])) value[--length] = '\0';
        if (result == 1 && !*value) choice = 1;
        else if (result == 1 && (!strcmp(value, "q") || !strcmp(value, "Q"))) choice = 0;
        else fputs("Usa Enter para reintentar o q para salir.\n", stderr);
    }
    return choice;
}
int input_fields(char *text, char **fields, size_t expected)
{
    char *save = NULL;
    char *field = strtok_r(text, " \t\r\n\v\f", &save);
    size_t count = 0;
    while (field && count < expected) {
        fields[count++] = field;
        field = strtok_r(NULL, " \t\r\n\v\f", &save);
    }
    return field || count != expected ? -1 : 0;
}
