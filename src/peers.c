#include "peers.h"
#include "sale.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int peers_load(const store_config *config, peer_config *other)
{
    FILE *file = fopen(config->peers_file, "r");
    if (!file) { perror("Catálogo de pares"); return -1; }
    char line[256];
    peer_config entries[2];
    size_t count = 0;
    int failed = 0;
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n') && !feof(file)) { failed = 1; break; }
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '#' || *start == '\n' || !*start) continue;
        char id[33], address[16], port_text[16], extra;
        if (count == 2 || sscanf(start, "%32s %15s %15s %c", id, address, port_text, &extra) != 3 ||
            !sale_identifier(id)) { failed = 1; break; }
        struct in_addr ipv4;
        if (inet_pton(AF_INET, address, &ipv4) != 1) { failed = 1; break; }
        for (char *p = port_text; *p; p++) if (*p < '0' || *p > '9') failed = 1;
        char *end;
        errno = 0;
        unsigned long port = strtoul(port_text, &end, 10);
        if (failed || errno || *end || !port || port > 65535) { failed = 1; break; }
        strcpy(entries[count].id, id);
        strcpy(entries[count].address, address);
        entries[count++].port = (unsigned int)port;
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    int own = -1;
    if (count == 2 && strcmp(entries[0].id, entries[1].id) &&
        (strcmp(entries[0].address, entries[1].address) || entries[0].port != entries[1].port)) {
        for (size_t i = 0; i < 2; i++) if (!strcmp(entries[i].id, config->store_id)) own = (int)i;
    }
    if (failed || own < 0 || entries[own].port != config->port ||
        (strcmp(config->bind_address, "0.0.0.0") && strcmp(entries[own].address, config->bind_address))) {
        fputs("Catálogo inválido: esta etapa requiere exactamente dos identidades y direcciones distintas, incluida la propia.\n", stderr);
        return -1;
    }
    *other = entries[1 - own];
    return 0;
}
