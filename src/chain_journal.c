#include "chain_journal.h"
#include "peers.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct chain_data {
    sale_record *records;
    size_t capacity;
    size_t *index;
    size_t index_capacity;
    char period[33];
    char other_id[33];
};
static const char header[] = "identificador,fecha_hora,valor_venta,crc32\n";

static int durable(int fd)
{
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    return result;
}

static int write_all(int fd, const void *data, size_t length)
{
    const char *bytes = data;
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, bytes + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

static int regular(int fd)
{
    struct stat info;
    if (fstat(fd, &info) < 0) return -1;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) { errno = EINVAL; return -1; }
    return 0;
}

static int directories(const char *path)
{
    char copy[256];
    strcpy(copy, path);
    size_t length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/') copy[--length] = '\0';
    for (size_t i = 1; i <= length; i++) {
        if (i != length && copy[i] != '/') continue;
        char saved = copy[i]; copy[i] = '\0';
        if (mkdir(copy, 0700) < 0 && errno != EEXIST) return -1;
        int fd = open(copy, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return -1;
        int result = durable(fd);
        if (close(fd) < 0) result = -1;
        if (result < 0) return -1;
        char parent[256]; strcpy(parent, copy);
        char *slash = strrchr(parent, '/');
        if (!slash) strcpy(parent, ".");
        else if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
        fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) return -1;
        result = durable(fd);
        if (close(fd) < 0) result = -1;
        if (result < 0) return -1;
        copy[i] = saved;
    }
    return 0;
}

static size_t find(const sales_journal *journal, const char *id)
{
    const struct chain_data *data = journal->chain;
    if (!data->index_capacity) return SIZE_MAX;
    size_t slot = (size_t)sale_crc(id, strlen(id)) & (data->index_capacity - 1);
    while (data->index[slot]) {
        size_t record = data->index[slot] - 1;
        if (!strcmp(data->records[record].id, id)) return record;
        slot = (slot + 1) & (data->index_capacity - 1);
    }
    return SIZE_MAX;
}

static int reserve(sales_journal *journal)
{
    struct chain_data *data = journal->chain;
    size_t count = (size_t)journal->count;
    if (count == data->capacity) {
        size_t capacity = data->capacity ? data->capacity * 2 : 64;
        if (capacity < data->capacity || capacity > SIZE_MAX / sizeof *data->records) { errno = ENOMEM; return -1; }
        sale_record *records = realloc(data->records, capacity * sizeof *records);
        if (!records) return -1;
        data->records = records; data->capacity = capacity;
    }
    if (count + 1 >= data->index_capacity / 2) {
        size_t capacity = data->index_capacity ? data->index_capacity * 2 : 128;
        if (capacity < data->index_capacity || capacity > SIZE_MAX / sizeof(size_t)) { errno = ENOMEM; return -1; }
        size_t *index = calloc(capacity, sizeof *index);
        if (!index) return -1;
        for (size_t i = 0; i < count; i++) {
            size_t slot = (size_t)sale_crc(data->records[i].id, strlen(data->records[i].id)) & (capacity - 1);
            while (index[slot]) slot = (slot + 1) & (capacity - 1);
            index[slot] = i + 1;
        }
        free(data->index); data->index = index; data->index_capacity = capacity;
    }
    return 0;
}

static void remember(sales_journal *journal, const sale_record *sale)
{
    struct chain_data *data = journal->chain;
    size_t position = (size_t)journal->count;
    data->records[position] = *sale;
    size_t slot = (size_t)sale_crc(sale->id, strlen(sale->id)) & (data->index_capacity - 1);
    while (data->index[slot]) slot = (slot + 1) & (data->index_capacity - 1);
    data->index[slot] = position + 1;
    journal->count++;
    journal->total_cent += sale->amount;
    if (!strcmp(sale->origin, journal->store_id)) {
        journal->own_total_cent += sale->amount;
        const char *period = strchr(sale->id, ':') + 1;
        if (!strncmp(period, data->period, 32) && sale->sequence > journal->own_sequence)
            journal->own_sequence = sale->sequence;
    }
}

static int allowed(const sales_journal *journal, const sale_record *sale)
{
    return (!strcmp(sale->origin, journal->store_id) || !strcmp(sale->origin, journal->chain->other_id)) &&
           sale->amount > 0 && sale->amount <= INT64_MAX - journal->total_cent && journal->count < SIZE_MAX;
}

static int load(sales_journal *journal)
{
    if (lseek(journal->fd, 0, SEEK_SET) < 0) return -1;
    int copy = dup(journal->fd);
    if (copy < 0) return -1;
    FILE *file = fdopen(copy, "r");
    if (!file) { close(copy); return -1; }
    char line[SALE_CSV_SIZE];
    int failed = 0;
    if (!fgets(line, sizeof line, file) || strcmp(line, header)) failed = 1;
    off_t valid_end = (off_t)strlen(header);
    while (!failed && fgets(line, sizeof line, file)) {
        if (!strchr(line, '\n')) {
            if (!feof(file)) { failed = 1; break; }
            fputs("Recuperación: último registro incompleto de cadena eliminado.\n", stderr);
            if (ftruncate(journal->fd, valid_end) < 0) failed = 1;
            break;
        }
        sale_record sale;
        if (sale_decode(line, &sale) < 0 || !allowed(journal, &sale) || find(journal, sale.id) != SIZE_MAX ||
            reserve(journal) < 0) { failed = 1; break; }
        remember(journal, &sale);
        valid_end += (off_t)strlen(line);
    }
    if (ferror(file)) failed = 1;
    if (fclose(file)) failed = 1;
    if (failed) { fputs("Historial de cadena inválido; no se abre la tienda.\n", stderr); errno = EINVAL; return -1; }
    return durable(journal->fd) < 0 || lseek(journal->fd, 0, SEEK_END) < 0 ? -1 : 0;
}

static int period_file(int directory, struct chain_data *data, int fresh)
{
    char line[64];
    if (fresh) {
        unsigned char random[16];
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        size_t done = 0;
        while (done < sizeof random) {
            ssize_t n = read(fd, random + done, sizeof random - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { close(fd); errno = EIO; return -1; }
            done += (size_t)n;
        }
        if (close(fd) < 0) return -1;
        for (size_t i = 0; i < sizeof random; i++) snprintf(data->period + i * 2, 3, "%02x", random[i]);
        int length = snprintf(line, sizeof line, "%s,%08" PRIx32 "\n", data->period, sale_crc(data->period, 32));
        fd = openat(directory, ".period.pending", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) return -1;
        int result = regular(fd) < 0 || ftruncate(fd, 0) < 0 || write_all(fd, line, (size_t)length) < 0 || durable(fd) < 0 ? -1 : 0;
        if (close(fd) < 0) result = -1;
        if (result < 0 || renameat(directory, ".period.pending", directory, "period.id") < 0 || durable(directory) < 0)
            return -1;
        return 0;
    }
    int fd = openat(directory, "period.id", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (regular(fd) < 0) { close(fd); return -1; }
    ssize_t n = read(fd, line, sizeof line);
    if (close(fd) < 0) return -1;
    if (n != 42 || line[32] != ',' || line[41] != '\n') { errno = EINVAL; return -1; }
    memcpy(data->period, line, 32); data->period[32] = '\0';
    for (size_t i = 0; i < 32; i++)
        if (!((data->period[i] >= '0' && data->period[i] <= '9') ||
              (data->period[i] >= 'a' && data->period[i] <= 'f'))) { errno = EINVAL; return -1; }
    char expected[64];
    snprintf(expected, sizeof expected, "%s,%08" PRIx32 "\n", data->period, sale_crc(data->period, 32));
    if (memcmp(line, expected, 42)) { errno = EINVAL; return -1; }
    return 0;
}

int chain_open(sales_journal *journal, const store_config *config)
{
    *journal = (sales_journal){ .fd = -1, .metadata_fd = -1, .legacy_fd = -1 };
    strcpy(journal->store_id, config->store_id);
    peer_config peer;
    if (peers_load(config, &peer) < 0) return -1;
    journal->chain = calloc(1, sizeof *journal->chain);
    if (!journal->chain) return -1;
    strcpy(journal->chain->other_id, peer.id);
    int directory = -1, temporary = -1;
    if (directories(config->data_dir) < 0) goto failure;
    directory = open(config->data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) goto failure;
    journal->metadata_fd = openat(directory, "store.meta", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (journal->metadata_fd < 0 || regular(journal->metadata_fd) < 0) goto failure;
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(journal->metadata_fd, F_SETLK, &lock) < 0) { fputs("Otra instancia usa la tienda.\n", stderr); goto failure; }
    char metadata[200], stored[200];
    int prefix = snprintf(metadata, sizeof metadata, "BLACKOUT_CHAIN_V1,%s,%s,%" PRId64,
                          config->store_id, config->chain_id, config->limit_cent);
    int tail = snprintf(metadata + prefix, sizeof metadata - (size_t)prefix, ",%08" PRIx32 "\n",
                        sale_crc(metadata, (size_t)prefix));
    size_t length = (size_t)(prefix + tail);
    ssize_t n;
    do { n = read(journal->metadata_fd, stored, sizeof stored); } while (n < 0 && errno == EINTR);
    if (n < 0) goto failure;
    if (n && ((size_t)n != length || memcmp(metadata, stored, length))) {
        fputs("Metadatos incompatibles: identidad, cadena o límite distintos.\n", stderr); errno = EINVAL; goto failure;
    }
    journal->fd = openat(directory, "sales.csv", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (journal->fd < 0 && errno != ENOENT) goto failure;
    if (journal->fd >= 0 && !n) { errno = EINVAL; goto failure; }
    int fresh = journal->fd < 0;
    if (period_file(directory, journal->chain, fresh) < 0) goto failure;
    if (fresh) {
        temporary = openat(directory, ".chain.csv.pending", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (temporary < 0 || regular(temporary) < 0 || ftruncate(temporary, 0) < 0 ||
            write_all(temporary, header, strlen(header)) < 0 || durable(temporary) < 0) goto failure;
        if (!n && (write_all(journal->metadata_fd, metadata, length) < 0 || durable(journal->metadata_fd) < 0)) goto failure;
        if (renameat(directory, ".chain.csv.pending", directory, "sales.csv") < 0 || durable(directory) < 0) goto failure;
        journal->fd = temporary; temporary = -1;
    }
    if (regular(journal->fd) < 0 || durable(journal->metadata_fd) < 0 || durable(directory) < 0 || load(journal) < 0)
        goto failure;
    if (close(directory) < 0) { directory = -1; goto failure; }
    return 0;
failure:
    perror("Abrir historial de cadena");
    if (directory >= 0) close(directory);
    if (temporary >= 0) close(temporary);
    chain_close(journal);
    return -1;
}

int chain_receive(sales_journal *journal, const sale_record *sale)
{
    size_t found = find(journal, sale->id);
    if (found != SIZE_MAX) {
        const sale_record *existing = &journal->chain->records[found];
        return existing->amount == sale->amount && !strcmp(existing->stamp, sale->stamp) ? 1 : 2;
    }
    if (!allowed(journal, sale)) return 2;
    if (reserve(journal) < 0) return -1;
    char line[SALE_CSV_SIZE];
    int n = sale_encode(sale, line, sizeof line);
    if (n < 0 || write_all(journal->fd, line, (size_t)n) < 0 || durable(journal->fd) < 0) return -1;
    remember(journal, sale);
    return 0;
}

int chain_local(sales_journal *journal, int64_t amount)
{
    if (journal->own_sequence == UINT64_MAX) { errno = EOVERFLOW; return -1; }
    sale_record sale = { .amount = amount, .sequence = journal->own_sequence + 1 };
    strcpy(sale.origin, journal->store_id);
    snprintf(sale.id, sizeof sale.id, "%s:%s:%" PRIu64, journal->store_id, journal->chain->period, sale.sequence);
    if (sale_timestamp(sale.stamp, sizeof sale.stamp) < 0) return -1;
    return chain_receive(journal, &sale) == 0 ? 0 : -1;
}

int chain_record(const sales_journal *journal, size_t index, sale_record *sale)
{
    if (index >= journal->count) return -1;
    *sale = journal->chain->records[index]; return 0;
}

int chain_history(const sales_journal *journal)
{
    puts("Identificador | Fecha y hora de registro (zona incluida) | Valor de venta");
    for (size_t i = 0; i < journal->count; i++) {
        const sale_record *sale = &journal->chain->records[i];
        printf("%s | %s | %" PRId64 ".%02" PRId64 "\n", sale->id, sale->stamp, sale->amount / 100, sale->amount % 100);
    }
    if (!journal->count) puts("Sin ventas registradas.");
    return ferror(stdout) ? -1 : 0;
}

int chain_close(sales_journal *journal)
{
    int failed = 0;
    if (journal->fd >= 0 && close(journal->fd) < 0) failed = 1;
    if (journal->metadata_fd >= 0 && close(journal->metadata_fd) < 0) failed = 1;
    if (journal->chain) {
        free(journal->chain->records); free(journal->chain->index); free(journal->chain);
    }
    journal->chain = NULL;
    journal->fd = journal->metadata_fd = -1;
    return failed ? -1 : 0;
}
