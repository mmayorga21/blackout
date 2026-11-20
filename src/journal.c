#include "journal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

enum { HEADER_SIZE = 64, RECORD_SIZE = 32 };

/* Formato independiente del padding y del orden de bytes de las estructuras C. */
static void put64(unsigned char *dest, uint64_t value)
{
    for (unsigned int i = 0; i < 8; i++) {
        dest[i] = (unsigned char)(value & 255U);
        value >>= 8;
    }
}

static uint64_t get64(const unsigned char *source)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; i++) value |= (uint64_t)source[i] << (i * 8);
    return value;
}

static uint32_t crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (unsigned int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ ((crc & 1U) ? UINT32_C(0xedb88320) : 0U);
    }
    return ~crc;
}

static void seal(unsigned char *data, size_t length)
{
    uint32_t crc = crc32(data, length - 4);
    for (unsigned int i = 0; i < 4; i++) data[length - 4 + i] = (unsigned char)(crc >> (i * 8));
}

static int verified(const unsigned char *data, size_t length)
{
    unsigned char copy[HEADER_SIZE];
    memcpy(copy, data, length);
    seal(copy, length);
    return memcmp(copy, data, length) == 0;
}

static int sync_fd(int fd)
{
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    return result;
}

static int write_all(int fd, const unsigned char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t written = write(fd, data + done, length - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (written == 0) errno = EIO;
            return -1;
        }
        done += (size_t)written;
    }
    return 0;
}

/* Devuelve los bytes leídos; distingue EOF, lectura parcial y error. */
static ssize_t read_record(int fd, unsigned char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t received = read(fd, data + done, length - done);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0) return -1;
        if (received == 0) break;
        done += (size_t)received;
    }
    return (ssize_t)done;
}

static int sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int result = sync_fd(fd);
    int saved = errno;
    if (close(fd) < 0 && result == 0) return -1;
    errno = saved;
    return result;
}

static int make_directory(char *path)
{
    if (mkdir(path, 0700) == 0) {
        char parent[256];
        memcpy(parent, path, strlen(path) + 1);
        char *slash = strrchr(parent, '/');
        if (slash == NULL) strcpy(parent, ".");
        else if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
        return sync_directory(parent);
    }
    if (errno != EEXIST) return -1;
    struct stat info;
    if (stat(path, &info) < 0) return -1;
    if (!S_ISDIR(info.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static int make_directories(const char *path)
{
    char copy[256];
    memcpy(copy, path, strlen(path) + 1);
    size_t length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/') copy[--length] = '\0';
    for (size_t i = 1; i < length; i++) {
        if (copy[i] != '/') continue;
        copy[i] = '\0';
        int result = make_directory(copy);
        copy[i] = '/';
        if (result < 0) return -1;
    }
    return make_directory(copy);
}

static void header_for(unsigned char *header, const store_config *config)
{
    memset(header, 0, HEADER_SIZE);
    memcpy(header, "BLKOUT1\n", 8);
    memcpy(header + 8, config->store_id, strlen(config->store_id));
    put64(header + 40, (uint64_t)config->limit_cent);
    seal(header, HEADER_SIZE);
}

static const char csv_header[] = "identificador,fecha_hora,valor_venta,crc32\n";

typedef struct {
    int fd;
    off_t next;
    size_t used, available;
    unsigned char data[4096];
} csv_reader;

/* 1: línea completa; 0: EOF; 2: cola incompleta; -1: error o línea inválida. */
static int csv_line(csv_reader *reader, char *line, size_t capacity, size_t *length)
{
    *length = 0;
    while (*length + 1 < capacity) {
        if (reader->used == reader->available) {
            ssize_t n;
            do { n = pread(reader->fd, reader->data, sizeof reader->data, reader->next); }
            while (n < 0 && errno == EINTR);
            if (n < 0) return -1;
            if (n == 0) { line[*length] = '\0'; return *length ? 2 : 0; }
            reader->next += n;
            reader->available = (size_t)n;
            reader->used = 0;
        }
        unsigned char c = reader->data[reader->used++];
        if (c == '\0') { errno = EINVAL; return -1; }
        line[(*length)++] = (char)c;
        if (c == '\n') { line[*length] = '\0'; return 1; }
    }
    errno = EOVERFLOW;
    return -1;
}

static int positive_integer(const char *text, uint64_t maximum, uint64_t *value)
{
    if (!*text) return -1;
    for (const char *p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end;
    uintmax_t parsed = strtoumax(text, &end, 10);
    if (errno || *end || parsed > maximum) return -1;
    *value = (uint64_t)parsed;
    return 0;
}

static int timestamp_valid(const char *text)
{
    if (strcmp(text, "desconocida") == 0) return 1;
    if (strlen(text) != 24) return 0;
    for (size_t i = 0; i < 24; i++) {
        if (i == 4 || i == 7) { if (text[i] != '-') return 0; }
        else if (i == 10) { if (text[i] != 'T') return 0; }
        else if (i == 13 || i == 16) { if (text[i] != ':') return 0; }
        else if (i == 19) { if (text[i] != '+' && text[i] != '-') return 0; }
        else if (text[i] < '0' || text[i] > '9') return 0;
    }
    unsigned int month, day, hour, minute, second, zone_hour, zone_minute;
    if (sscanf(text + 5, "%2u-%2uT%2u:%2u:%2u", &month, &day, &hour, &minute, &second) != 5 ||
        sscanf(text + 20, "%2u%2u", &zone_hour, &zone_minute) != 2) return 0;
    return month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 &&
           minute <= 59 && second <= 60 && zone_hour <= 23 && zone_minute <= 59;
}

static int sale_line(char *line, size_t capacity, const char *store_id, uint64_t sequence,
                     const char *stamp, int64_t amount)
{
    int length = snprintf(line, capacity, "%s:%" PRIu64 ",%s,%" PRId64 ".%02" PRId64,
                          store_id, sequence, stamp, amount / 100, amount % 100);
    if (length < 0 || (size_t)length >= capacity) { errno = EOVERFLOW; return -1; }
    uint32_t crc = crc32((const unsigned char *)line, (size_t)length);
    int suffix = snprintf(line + length, capacity - (size_t)length, ",%08" PRIx32 "\n", crc);
    if (suffix < 0 || (size_t)suffix >= capacity - (size_t)length) { errno = EOVERFLOW; return -1; }
    return length + suffix;
}

static int decode_sale(char *line, const char *store_id, uint64_t *sequence,
                       char *stamp, int64_t *amount)
{
    size_t length = strlen(line);
    if (!length || line[length - 1] != '\n') return -1;
    line[--length] = '\0';
    char *checksum = strrchr(line, ',');
    if (!checksum || strlen(checksum + 1) != 8) return -1;
    char expected[9];
    snprintf(expected, sizeof expected, "%08" PRIx32,
             crc32((const unsigned char *)line, (size_t)(checksum - line)));
    if (strcmp(checksum + 1, expected) != 0) return -1;
    *checksum = '\0';
    char *date = strchr(line, ',');
    if (!date) return -1;
    *date++ = '\0';
    char *value = strchr(date, ',');
    if (!value) return -1;
    *value++ = '\0';
    if (!timestamp_valid(date)) return -1;
    char *colon = strchr(line, ':');
    if (!colon) return -1;
    *colon++ = '\0';
    if (strcmp(line, store_id) != 0 || positive_integer(colon, UINT64_MAX, sequence) < 0 || !*sequence)
        return -1;
    char *point = strchr(value, '.');
    if (!point || strlen(point + 1) != 2) return -1;
    *point++ = '\0';
    uint64_t whole, fraction;
    if (positive_integer(value, INT64_MAX / 100, &whole) < 0 ||
        positive_integer(point, 99, &fraction) < 0 || whole > ((uint64_t)INT64_MAX - fraction) / 100)
        return -1;
    *amount = (int64_t)(whole * 100 + fraction);
    if (*amount == 0) return -1;
    strcpy(stamp, date);
    return 0;
}

static int lock_regular(int fd)
{
    struct stat info;
    if (fstat(fd, &info) < 0) return -1;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) { errno = EINVAL; return -1; }
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        fputs("No se puede bloquear el historial: otra instancia puede estar usándolo.\n", stderr);
        return -1;
    }
    return 0;
}

/* El respaldo binario se valida y convierte sin escribir ni truncar su contenido. */
static int migrate_legacy(int source, int target, const store_config *config)
{
    unsigned char expected[HEADER_SIZE], data[HEADER_SIZE];
    header_for(expected, config);
    ssize_t n = read_record(source, data, HEADER_SIZE);
    if (n < 0) return -1;
    if (n != HEADER_SIZE || !verified(data, HEADER_SIZE) || memcmp(expected, data, HEADER_SIZE) != 0) {
        fputs("Cabecera binaria incompleta, dañada o incompatible; se conserva el original.\n", stderr);
        errno = EINVAL; return -1;
    }
    uint64_t count = 0;
    int64_t total = 0;
    while ((n = read_record(source, data, RECORD_SIZE)) > 0) {
        if (n != RECORD_SIZE) {
            fputs("Migración: registro incompleto final omitido; el respaldo no se modifica.\n", stderr);
            break;
        }
        uint64_t sequence = get64(data + 4), amount = get64(data + 12);
        if (!verified(data, RECORD_SIZE) || memcmp(data, "SALE", 4) || count == UINT64_MAX ||
            sequence != count + 1 || !amount || amount > (uint64_t)(INT64_MAX - total) ||
            total >= config->limit_cent) { errno = EINVAL; return -1; }
        char line[256];
        int length = sale_line(line, sizeof line, config->store_id, sequence, "desconocida", (int64_t)amount);
        if (length < 0 || write_all(target, (unsigned char *)line, (size_t)length) < 0) return -1;
        count = sequence;
        total += (int64_t)amount;
    }
    return n < 0 ? -1 : 0;
}

static int load_csv(sales_journal *journal, int64_t limit)
{
    csv_reader reader = { .fd = journal->fd };
    char line[256], stamp[32];
    size_t length;
    if (csv_line(&reader, line, sizeof line, &length) != 1 || strcmp(line, csv_header) != 0) {
        fputs("Cabecera CSV incompleta o dañada; no se modifica el historial.\n", stderr);
        errno = EINVAL; return -1;
    }
    off_t valid_end = (off_t)length;
    int result;
    while ((result = csv_line(&reader, line, sizeof line, &length)) == 1) {
        uint64_t sequence;
        int64_t amount;
        if (decode_sale(line, journal->store_id, &sequence, stamp, &amount) < 0 ||
            journal->count == UINT64_MAX || sequence != journal->count + 1 ||
            amount > INT64_MAX - journal->total_cent || journal->total_cent >= limit) {
            fputs("Historial corrupto o secuencia contable inválida; no se descartan registros.\n", stderr);
            errno = EINVAL; return -1;
        }
        journal->count = sequence;
        journal->total_cent += amount;
        valid_end += (off_t)length;
    }
    if (result < 0) return -1;
    if (result == 2) {
        fputs("Recuperación: se elimina únicamente el último registro incompleto sin salto de línea.\n", stderr);
        if (ftruncate(journal->fd, valid_end) < 0) return -1;
    }
    if (sync_fd(journal->fd) < 0 || lseek(journal->fd, 0, SEEK_END) < 0) return -1;
    return 0;
}

int journal_open(sales_journal *journal, const store_config *config)
{
    *journal = (sales_journal){ .fd = -1, .metadata_fd = -1, .legacy_fd = -1 };
    strcpy(journal->store_id, config->store_id);
    if (make_directories(config->data_dir) < 0) { perror("Directorio de datos"); return -1; }
    int directory = open(config->data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) { perror("Abrir directorio de datos"); return -1; }
    int temporary = -1;
    journal->metadata_fd = openat(directory, "store.meta", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (journal->metadata_fd < 0 || lock_regular(journal->metadata_fd) < 0) goto failure;
    char metadata[160], stored[160];
    int prefix = snprintf(metadata, sizeof metadata, "BLACKOUT_CSV_V1,%s,%" PRId64,
                          config->store_id, config->limit_cent);
    uint32_t crc = crc32((unsigned char *)metadata, (size_t)prefix);
    int suffix = snprintf(metadata + prefix, sizeof metadata - (size_t)prefix, ",%08" PRIx32 "\n", crc);
    size_t metadata_length = (size_t)(prefix + suffix);
    ssize_t metadata_bytes = read_record(journal->metadata_fd, (unsigned char *)stored, sizeof stored);
    if (metadata_bytes < 0) goto failure;
    if (metadata_bytes != 0 && ((size_t)metadata_bytes != metadata_length ||
                                memcmp(stored, metadata, metadata_length) != 0)) {
        fputs("Metadatos incompatibles o dañados: identidad o límite distintos.\n", stderr);
        errno = EINVAL; goto failure;
    }
    /* Mantener también el bloqueo antiguo mientras se usa su respaldo evita migrar sobre una instancia vieja. */
    journal->legacy_fd = openat(directory, "sales.journal", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (journal->legacy_fd < 0 && errno != ENOENT) goto failure;
    if (journal->legacy_fd >= 0 && lock_regular(journal->legacy_fd) < 0) goto failure;
    journal->fd = openat(directory, "sales.csv", O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (journal->fd < 0 && errno != ENOENT) goto failure;
    if (journal->fd >= 0 && metadata_bytes == 0) {
        fputs("CSV sin metadatos de identidad y límite: se conserva para diagnóstico.\n", stderr);
        errno = EINVAL; goto failure;
    }
    if (journal->fd < 0) {
        if (metadata_bytes != 0) {
            fputs("Falta el CSV de una tienda ya inicializada; no se crea un historial vacío.\n", stderr);
            errno = EINVAL; goto failure;
        }
        temporary = openat(directory, ".sales.csv.pending", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (temporary < 0 || lock_regular(temporary) < 0 || ftruncate(temporary, 0) < 0 ||
            write_all(temporary, (const unsigned char *)csv_header, strlen(csv_header)) < 0) goto failure;
        if (journal->legacy_fd >= 0 && migrate_legacy(journal->legacy_fd, temporary, config) < 0) goto failure;
        if (sync_fd(temporary) < 0) goto failure;
        if (metadata_bytes == 0 && (write_all(journal->metadata_fd, (unsigned char *)metadata, metadata_length) < 0 ||
                                   sync_fd(journal->metadata_fd) < 0)) goto failure;
        if (renameat(directory, ".sales.csv.pending", directory, "sales.csv") < 0 || sync_fd(directory) < 0)
            goto failure;
        journal->fd = temporary;
        temporary = -1;
        if (journal->legacy_fd >= 0)
            fputs("Historial convertido a sales.csv; sales.journal se conserva como respaldo sin fechas originales.\n", stderr);
    }
    if (lock_regular(journal->fd) < 0 || sync_fd(journal->metadata_fd) < 0 || sync_fd(directory) < 0 ||
        load_csv(journal, config->limit_cent) < 0) goto failure;
    if (close(directory) < 0) { directory = -1; goto failure; }
    return 0;

failure:
    perror("Abrir registro CSV");
    if (temporary >= 0) close(temporary);
    if (directory >= 0) close(directory);
    if (journal->fd >= 0) close(journal->fd);
    if (journal->legacy_fd >= 0) close(journal->legacy_fd);
    if (journal->metadata_fd >= 0) close(journal->metadata_fd);
    journal->fd = journal->metadata_fd = journal->legacy_fd = -1;
    return -1;
}

int journal_append(sales_journal *journal, int64_t amount)
{
    if (amount <= 0 || amount > INT64_MAX - journal->total_cent || journal->count == UINT64_MAX) {
        errno = EOVERFLOW; return -1;
    }
    time_t now = time(NULL);
    struct tm local;
    char stamp[32], line[256];
    if (now == (time_t)-1 || localtime_r(&now, &local) == NULL ||
        strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S%z", &local) == 0 || !timestamp_valid(stamp)) {
        errno = EINVAL; return -1;
    }
    int length = sale_line(line, sizeof line, journal->store_id, journal->count + 1, stamp, amount);
    if (length < 0 || write_all(journal->fd, (unsigned char *)line, (size_t)length) < 0 || sync_fd(journal->fd) < 0)
        return -1;
    journal->count++;
    journal->total_cent += amount;
    return 0;
}

int journal_history(const sales_journal *journal)
{
    csv_reader reader = { .fd = journal->fd };
    char line[256], stamp[32];
    size_t length;
    if (csv_line(&reader, line, sizeof line, &length) != 1 || strcmp(line, csv_header)) return -1;
    puts("Identificador | Fecha y hora de registro (zona incluida) | Valor de venta");
    uint64_t shown = 0;
    int result;
    while ((result = csv_line(&reader, line, sizeof line, &length)) == 1) {
        uint64_t sequence;
        int64_t amount;
        if (decode_sale(line, journal->store_id, &sequence, stamp, &amount) < 0 || sequence != shown + 1)
            return -1;
        printf("%s:%" PRIu64 " | %s | %" PRId64 ".%02" PRId64 "\n",
               journal->store_id, sequence, stamp, amount / 100, amount % 100);
        shown++;
    }
    if (result != 0 || shown != journal->count) return -1;
    if (shown == 0) puts("Sin ventas registradas.");
    return ferror(stdout) ? -1 : 0;
}

int journal_close(sales_journal *journal)
{
    int failed = 0;
    if (close(journal->fd) < 0) failed = 1;
    if (journal->legacy_fd >= 0 && close(journal->legacy_fd) < 0) failed = 1;
    if (close(journal->metadata_fd) < 0) failed = 1;
    journal->fd = journal->metadata_fd = journal->legacy_fd = -1;
    return failed ? -1 : 0;
}
