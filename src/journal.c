#include "journal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

int journal_open(sales_journal *journal, const store_config *config)
{
    *journal = (sales_journal){ .fd = -1 };
    if (make_directories(config->data_dir) < 0) { perror("Directorio de datos"); return -1; }
    int directory = open(config->data_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) { perror("Abrir directorio de datos"); return -1; }
    int fd = openat(directory, "sales.journal", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { perror("Abrir historial"); close(directory); return -1; }
    struct flock lock = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        fputs("No se puede bloquear el historial: otra instancia puede estar usándolo.\n", stderr);
        close(fd); close(directory); return -1;
    }
    struct stat info;
    unsigned char expected[HEADER_SIZE], data[HEADER_SIZE];
    header_for(expected, config);
    if (fstat(fd, &info) < 0) goto io_error;
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1) {
        fputs("El historial debe ser un archivo regular sin enlaces físicos adicionales.\n", stderr);
        goto failure;
    }
    if (info.st_size == 0) {
        if (write_all(fd, expected, HEADER_SIZE) < 0 || sync_fd(fd) < 0) goto io_error;
    } else {
        ssize_t bytes = read_record(fd, data, HEADER_SIZE);
        if (bytes < 0) goto io_error;
        if (bytes != HEADER_SIZE || !verified(data, HEADER_SIZE)) {
            fputs("Cabecera incompleta o dañada; no se modifica el historial.\n", stderr);
            goto failure;
        }
        if (memcmp(data, expected, HEADER_SIZE) != 0) {
            fputs("Historial incompatible: versión, identidad o límite distintos. No se modifica.\n", stderr);
            goto failure;
        }
    }
    /* También sincroniza el nombre cuando el archivo quedó creado en un intento anterior. */
    if (sync_fd(directory) < 0) goto io_error;
    off_t valid_end = HEADER_SIZE;
    ssize_t bytes;
    while ((bytes = read_record(fd, data, RECORD_SIZE)) > 0) {
        if (bytes != RECORD_SIZE) {
            fputs("Recuperación: se elimina únicamente el último registro incompleto.\n", stderr);
            if (ftruncate(fd, valid_end) < 0 || sync_fd(fd) < 0) goto io_error;
            break;
        }
        uint64_t sequence = get64(data + 4), amount = get64(data + 12);
        if (!verified(data, RECORD_SIZE) || memcmp(data, "SALE", 4) != 0 ||
            journal->count == UINT64_MAX || sequence != journal->count + 1 ||
            amount == 0 || amount > (uint64_t)(INT64_MAX - journal->total_cent) ||
            journal->total_cent >= config->limit_cent) {
            fputs("Historial corrupto o secuencia contable inválida; no se descartan registros.\n", stderr);
            goto failure;
        }
        journal->count = sequence;
        journal->total_cent += (int64_t)amount;
        valid_end += RECORD_SIZE;
    }
    /* Una cola completa pudo escribirse antes de caer, sin llegar al fsync ni al ACK. */
    if (bytes < 0 || sync_fd(fd) < 0 || lseek(fd, 0, SEEK_END) < 0) goto io_error;
    if (close(directory) < 0) { perror("Cerrar directorio"); close(fd); return -1; }
    journal->fd = fd;
    return 0;

io_error:
    perror("Error de almacenamiento");
failure:
    close(fd);
    close(directory);
    return -1;
}

int journal_append(sales_journal *journal, int64_t amount)
{
    if (amount <= 0 || amount > INT64_MAX - journal->total_cent || journal->count == UINT64_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    unsigned char record[RECORD_SIZE] = {0};
    memcpy(record, "SALE", 4);
    put64(record + 4, journal->count + 1);
    put64(record + 12, (uint64_t)amount);
    seal(record, RECORD_SIZE);
    if (write_all(journal->fd, record, RECORD_SIZE) < 0 || sync_fd(journal->fd) < 0) return -1;
    journal->count++;
    journal->total_cent += amount;
    return 0;
}

int journal_close(sales_journal *journal)
{
    int result = close(journal->fd);
    journal->fd = -1;
    return result;
}
