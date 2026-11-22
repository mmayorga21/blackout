#include "peers.h"
#include "sale.h"
#include "input.h"
#include "display.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int peers_load(const store_config *config, peer_catalog *catalog)
{
    FILE *file = fopen(config->peers_file, "r");
    if (!file) { display_perror("Catálogo de pares"); return -1; }
    char line[256];
    peer_config entries[PEERS_MAX];
    size_t count = 0;
    int failed = 0;
    while (fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n') && !feof(file)) { failed = 1; break; }
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '#' || *start == '\n' || !*start) continue;
        char *fields[3];
        if (count == PEERS_MAX || input_fields(start, fields, 3) < 0 || !sale_identifier(fields[0])) { failed = 1; break; }
        const char *id = fields[0], *address = fields[1], *port_text = fields[2];
        struct in_addr ipv4;
        if (strlen(address) >= sizeof entries[0].address || inet_pton(AF_INET, address, &ipv4) != 1) { failed = 1; break; }
        for (const char *p = port_text; *p; p++) if (*p < '0' || *p > '9') failed = 1;
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
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(entries[i].id, config->store_id)) own = (int)i;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(entries[i].id, entries[j].id) ||
                (!strcmp(entries[i].address, entries[j].address) && entries[i].port == entries[j].port)) failed = 1;
    }
    if (failed || own < 0 || entries[own].port != config->port ||
        (strcmp(config->bind_address, "0.0.0.0") && strcmp(entries[own].address, config->bind_address))) {
        fputs("Catálogo inválido: identidades y direcciones únicas, incluida la propia (máximo 32 tiendas).\n", stderr);
        return -1;
    }
    memcpy(catalog->entries, entries, count * sizeof *entries);
    catalog->count = count; catalog->own = (size_t)own;
    return 0;
}
