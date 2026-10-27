// SPDX-License-Identifier: Apache-2.0
#include "project_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(APP_PROJECT_IO_INTERPOSE)
#include "project_io_faults.h"
#endif

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define file_sync _commit
#define file_number _fileno
#define make_directory(path) _mkdir(path)
#else
#include <dirent.h>
#include <unistd.h>
#define file_sync fsync
#define file_number fileno
#define make_directory(path) mkdir(path, 0700)
#endif
#if defined(ESP_PLATFORM)
#include "app_nand.h"
#endif

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

uint32_t project_crc32(const void *bytes, size_t size)
{
    const uint8_t *data = bytes;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
    }
    return ~crc;
}

bool project_name_valid(const char *name)
{
    if (!name || !name[0] || memchr(name, 0, APP_PROJECT_NAME_BYTES) == NULL) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\') return false;
    }
    return true;
}
bool project_id_valid(const char *id)
{
    if (!id || strlen(id) != 32) return false;
    return strspn(id, "0123456789abcdef") == 32;
}
bool project_filename_valid(const char *name)
{
    if (!name || !name[0] || memchr(name, 0, APP_PROJECT_FILENAME_BYTES) == NULL ||
        !strcmp(name, ".") || !strcmp(name, "..")) return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (*p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':') return false;
    }
    return true;
}
app_project_error_t project_errno_error(void)
{
    return errno == ENOSPC ? APP_PROJECT_NO_SPACE : errno == ENOENT ? APP_PROJECT_NOT_FOUND
           : errno == ENOMEM ? APP_PROJECT_NO_MEMORY : APP_PROJECT_IO_ERROR;
}
bool project_path(char out[PROJECT_PATH_BYTES], const char *base, const char *name)
{
    const int length = snprintf(out, PROJECT_PATH_BYTES, "%s/%s", base, name);
    return length > 0 && length < PROJECT_PATH_BYTES;
}

static bool directory(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}
static app_project_error_t mkdir_one(const char *path)
{
    if (make_directory(path) == 0 || (errno == EEXIST && directory(path))) return APP_PROJECT_OK;
    return project_errno_error();
}
static app_project_error_t mkdir_tree(const char *path)
{
    char *copy = malloc(strlen(path) + 1U);
    if (!copy) return APP_PROJECT_NO_MEMORY;
    strcpy(copy, path);
    app_project_error_t error = APP_PROJECT_OK;
    for (char *p = copy + 1; *p && error == APP_PROJECT_OK; ++p) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':') continue;
        const char separator = *p;
        *p = 0;
        error = mkdir_one(copy);
        *p = separator;
    }
    if (error == APP_PROJECT_OK) error = mkdir_one(copy);
    free(copy);
    return error;
}

app_project_error_t project_prepare_storage(app_project_t *p)
{
#if defined(ESP_PLATFORM)
    const esp_err_t mounted = app_nand_mount();
    if (mounted != ESP_OK) { p->snapshot.io_error = mounted; return APP_PROJECT_STORAGE_UNAVAILABLE; }
    p->owns_nand = true;
#endif
    app_project_error_t error = mkdir_tree(p->root);
    if (error == APP_PROJECT_OK) error = mkdir_one(p->projects);
    if (error == APP_PROJECT_OK) error = mkdir_one(p->inbox);
    project_platform_lock(&p->platform);
    p->inbox_ready = error == APP_PROJECT_OK;
    p->snapshot.storage_ready = error == APP_PROJECT_OK;
    project_platform_unlock(&p->platform);
    return error;
}

static app_project_error_t read_bytes(const char *path, uint8_t **bytes, size_t *size, size_t maximum)
{
    *bytes = NULL; *size = 0;
    struct stat st;
    if (stat(path, &st)) return project_errno_error();
    if ((st.st_mode & S_IFMT) != S_IFREG || st.st_size < 0 ||
        (uint64_t)st.st_size > maximum) return APP_PROJECT_CORRUPT;
    FILE *file = fopen(path, "rb");
    if (!file) return project_errno_error();
    const size_t length = (size_t)st.st_size;
    uint8_t *data = malloc(length ? length : 1U);
    if (!data) { fclose(file); return APP_PROJECT_NO_MEMORY; }
    app_project_error_t error = APP_PROJECT_OK;
    if (fread(data, 1, length, file) != length || fgetc(file) != EOF || ferror(file)) error = APP_PROJECT_IO_ERROR;
    if (fclose(file) && error == APP_PROJECT_OK) error = project_errno_error();
    if (error != APP_PROJECT_OK) { free(data); return error; }
    *bytes = data; *size = length;
    return APP_PROJECT_OK;
}
app_project_error_t project_read_inbox(app_project_t *p, const char *filename,
                                       uint8_t **bytes, size_t *size, size_t maximum)
{
    if (!project_filename_valid(filename)) return APP_PROJECT_INVALID;
    char path[PROJECT_PATH_BYTES];
    if (!project_path(path, p->inbox, filename)) return APP_PROJECT_INVALID;
    return read_bytes(path, bytes, size, maximum);
}

static app_project_error_t write_bytes(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file) return project_errno_error();
    app_project_error_t error = APP_PROJECT_OK;
    if (fwrite(data, 1, size, file) != size || fflush(file) || file_sync(file_number(file))) error = project_errno_error();
    if (fclose(file) && error == APP_PROJECT_OK) error = project_errno_error();
    return error;
}
static app_project_error_t verify_file(const char *path, size_t expected_size, uint32_t expected_crc)
{
    FILE *file = fopen(path, "rb");
    if (!file) return project_errno_error();
    uint8_t *buffer = malloc(4096);
    if (!buffer) { fclose(file); return APP_PROJECT_NO_MEMORY; }
    uint32_t crc = UINT32_MAX;
    size_t total = 0;
    app_project_error_t error = APP_PROJECT_OK;
    while (total < expected_size) {
        const size_t count = expected_size - total < 4096 ? expected_size - total : 4096;
        if (fread(buffer, 1, count, file) != count) { error = ferror(file) ? APP_PROJECT_IO_ERROR : APP_PROJECT_CORRUPT; break; }
        for (size_t index = 0; index < count; ++index) {
            crc ^= buffer[index];
            for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
        }
        total += count;
    }
    if (error == APP_PROJECT_OK && (fgetc(file) != EOF || ferror(file) || ~crc != expected_crc)) error = APP_PROJECT_CORRUPT;
    if (fclose(file) && error == APP_PROJECT_OK) error = project_errno_error();
    free(buffer);
    return error;
}
static app_project_error_t sync_directory(const char *path)
{
#if defined(_WIN32) || defined(ESP_PLATFORM)
    /* Windows MOVEFILE_WRITE_THROUGH and LittleFS metadata commits provide
     * their platform boundary; POSIX hosts explicitly fsync the directory. */
    (void)path;
    return APP_PROJECT_OK;
#else
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return project_errno_error();
    const int result = fsync(fd);
    const int closed = close(fd);
    return result || closed ? project_errno_error() : APP_PROJECT_OK;
#endif
}
static app_project_error_t replace_file(const char *temporary, const char *target)
{
#if defined(_WIN32)
    if (!MoveFileExA(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return APP_PROJECT_IO_ERROR;
#else
    if (rename(temporary, target)) return project_errno_error();
#endif
    return APP_PROJECT_OK;
}
static app_project_error_t pointer_read(const char *base, const char *which, char token[19])
{
    char path[PROJECT_PATH_BYTES];
    if (!project_path(path, base, which)) return APP_PROJECT_INVALID;
    uint8_t *bytes = NULL;
    size_t size = 0;
    app_project_error_t error = read_bytes(path, &bytes, &size, 19);
    if (error == APP_PROJECT_OK) {
        if (size != 19 || bytes[0] != 'g' || bytes[1] != '-' || bytes[18] != '\n') error = APP_PROJECT_CORRUPT;
        else {
            memcpy(token, bytes, 18); token[18] = 0;
            if (strspn(token + 2, "0123456789abcdef") != 16) error = APP_PROJECT_CORRUPT;
        }
    }
    free(bytes);
    return error;
}
static app_project_error_t pointer_write(const char *base, const char *which, const char *token)
{
    char *paths = malloc(PROJECT_PATH_BYTES * 2U);
    if (!paths) return APP_PROJECT_NO_MEMORY;
    char *path = paths, *temporary = paths + PROJECT_PATH_BYTES;
    char name[32];
    const int n = snprintf(name, sizeof(name), "%s.tmp", which);
    app_project_error_t error = n > 0 && n < (int)sizeof(name) && project_path(path, base, which) &&
                              project_path(temporary, base, name) ? APP_PROJECT_OK : APP_PROJECT_INVALID;
    char bytes[20];
    if (error == APP_PROJECT_OK) {
        snprintf(bytes, sizeof(bytes), "%s\n", token);
        error = write_bytes(temporary, (const uint8_t *)bytes, 19);
    }
    if (error == APP_PROJECT_OK) error = verify_file(temporary, 19, project_crc32(bytes, 19));
    if (error == APP_PROJECT_OK) error = replace_file(temporary, path);
    if (error == APP_PROJECT_OK) error = sync_directory(base);
    free(paths);
    return error;
}

static app_project_error_t generation_load(app_project_t *p, const char *id,
                                           const char *token, project_payload_t *payload,
                                           bool metadata_only)
{
    char *paths = malloc(PROJECT_PATH_BYTES * 3U);
    if (!paths) return APP_PROJECT_NO_MEMORY;
    char *base = paths, *generation = paths + PROJECT_PATH_BYTES, *path = paths + PROJECT_PATH_BYTES * 2U;
    app_project_error_t error = project_path(base, p->projects, id) && project_path(generation, base, token) &&
                               project_path(path, generation, "manifest.bin") ? APP_PROJECT_OK : APP_PROJECT_INVALID;
    uint8_t *manifest = NULL;
    size_t size = 0;
    if (error == APP_PROJECT_OK) error = read_bytes(path, &manifest, &size, PROJECT_MANIFEST_BYTES);
    if (error == APP_PROJECT_OK && (size != PROJECT_MANIFEST_BYTES || memcmp(manifest, "OPAYPRJ1", 8) ||
        get32(manifest + 8) != 1 || get32(manifest + 188) != project_crc32(manifest, 188) ||
        memchr(manifest + 56, 0, APP_PROJECT_NAME_BYTES) == NULL ||
        memchr(manifest + 120, 0, APP_PROJECT_ID_BYTES) == NULL || strcmp((char *)manifest + 120, id) ||
        !project_name_valid((char *)manifest + 56) || get64(manifest + 28) > PROJECT_MAX_MODULE_BYTES ||
        get64(manifest + 36) > PROJECT_MAX_BANK_BYTES)) error = APP_PROJECT_CORRUPT;
    if (error == APP_PROJECT_OK) {
        payload->generation = get64(manifest + 12);
        payload->revision = get64(manifest + 20);
        strcpy(payload->id, id); strcpy(payload->name, (char *)manifest + 56);
        payload->module_size = (size_t)get64(manifest + 28);
        payload->bank_size = (size_t)get64(manifest + 36);
    }
    if (error == APP_PROJECT_OK && !metadata_only) {
        if (!project_path(path, generation, "module.s3m")) error = APP_PROJECT_INVALID;
        size = 0;
        if (error == APP_PROJECT_OK) error = read_bytes(path, &payload->module, &size, PROJECT_MAX_MODULE_BYTES);
        if (error == APP_PROJECT_OK && (size != payload->module_size || project_crc32(payload->module, size) != get32(manifest + 44))) error = APP_PROJECT_CORRUPT;
        if (error == APP_PROJECT_OK && !project_path(path, generation, "samples.bank")) error = APP_PROJECT_INVALID;
        size = 0;
        if (error == APP_PROJECT_OK) error = read_bytes(path, &payload->bank_bytes, &size, PROJECT_MAX_BANK_BYTES);
        if (error == APP_PROJECT_OK && (size != payload->bank_size || project_crc32(payload->bank_bytes, size) != get32(manifest + 48))) error = APP_PROJECT_CORRUPT;
        uint8_t *state = NULL;
        if (error == APP_PROJECT_OK && !project_path(path, generation, "state.bin")) error = APP_PROJECT_INVALID;
        size = 0;
        if (error == APP_PROJECT_OK) error = read_bytes(path, &state, &size, PROJECT_STATE_BYTES);
        if (error == APP_PROJECT_OK && (size != PROJECT_STATE_BYTES || project_crc32(state, size) != get32(manifest + 52) ||
            !project_state_decode(state, &payload->state))) error = APP_PROJECT_CORRUPT;
        free(state);
    }
    free(manifest); free(paths);
    return error;
}

app_project_error_t project_file_load(app_project_t *p, const char *id, project_payload_t *payload)
{
    if (!project_id_valid(id)) return APP_PROJECT_INVALID;
    char base[PROJECT_PATH_BYTES], token[19];
    if (!project_path(base, p->projects, id)) return APP_PROJECT_INVALID;
    app_project_error_t error = pointer_read(base, "CURRENT", token);
    if (error == APP_PROJECT_OK) error = generation_load(p, id, token, payload, false);
    if (error == APP_PROJECT_CORRUPT || error == APP_PROJECT_NOT_FOUND) {
        const app_project_error_t original_error = error;
        project_payload_destroy(payload);
        error = pointer_read(base, "PREVIOUS", token);
        if (error == APP_PROJECT_OK) error = generation_load(p, id, token, payload, false);
        else if (error == APP_PROJECT_NOT_FOUND && original_error == APP_PROJECT_CORRUPT) {
            /* A damaged first generation is a corrupt project, not a missing
             * project merely because it has no prior fallback yet. */
            error = original_error;
        }
    }
    return error;
}

app_project_error_t project_file_save(app_project_t *p, project_payload_t *payload)
{
    if (!project_id_valid(payload->id) || !project_name_valid(payload->name)) return APP_PROJECT_INVALID;
    char *paths = malloc(PROJECT_PATH_BYTES * 3U);
    uint8_t *manifest = calloc(1, PROJECT_MANIFEST_BYTES);
    uint8_t *state = calloc(1, PROJECT_STATE_BYTES);
    if (!paths || !manifest || !state) { free(paths); free(manifest); free(state); return APP_PROJECT_NO_MEMORY; }
    char *base = paths, *generation = paths + PROJECT_PATH_BYTES, *path = paths + PROJECT_PATH_BYTES * 2U;
    app_project_error_t error = project_path(base, p->projects, payload->id) ? mkdir_one(base) : APP_PROJECT_INVALID;
    char token[19];
    if (error == APP_PROJECT_OK) {
        /* Immutable generations are never reused. A failed prior save remains
         * an orphan and cannot damage CURRENT or the verified old generation. */
        for (unsigned attempt = 0; attempt < 1024; ++attempt) {
            payload->generation = ++p->next_token;
            snprintf(token, sizeof(token), "g-%016" PRIx64, payload->generation);
            if (!project_path(generation, base, token)) { error = APP_PROJECT_INVALID; break; }
            if (make_directory(generation) == 0) break;
            if (errno != EEXIST) { error = project_errno_error(); break; }
            if (attempt == 1023) error = APP_PROJECT_IO_ERROR;
        }
    }
    if (error == APP_PROJECT_OK && !project_state_encode(&payload->state, state)) error = APP_PROJECT_INVALID;
    const char *const filenames[3] = {"module.s3m", "samples.bank", "state.bin"};
    const uint8_t *data[3] = {payload->module, payload->bank_bytes, state};
    const size_t sizes[3] = {payload->module_size, payload->bank_size, PROJECT_STATE_BYTES};
    for (unsigned index = 0; index < 3 && error == APP_PROJECT_OK; ++index) {
        if (!project_path(path, generation, filenames[index])) error = APP_PROJECT_INVALID;
        else error = write_bytes(path, data[index], sizes[index]);
        if (error == APP_PROJECT_OK) error = verify_file(path, sizes[index], project_crc32(data[index], sizes[index]));
    }
    if (error == APP_PROJECT_OK) {
        memcpy(manifest, "OPAYPRJ1", 8); put32(manifest + 8, 1);
        put64(manifest + 12, payload->generation); put64(manifest + 20, payload->revision);
        put64(manifest + 28, payload->module_size); put64(manifest + 36, payload->bank_size);
        put32(manifest + 44, project_crc32(payload->module, payload->module_size));
        put32(manifest + 48, project_crc32(payload->bank_bytes, payload->bank_size));
        put32(manifest + 52, project_crc32(state, PROJECT_STATE_BYTES));
        strcpy((char *)manifest + 56, payload->name); strcpy((char *)manifest + 120, payload->id);
        put32(manifest + 188, project_crc32(manifest, 188));
        if (!project_path(path, generation, "manifest.bin")) error = APP_PROJECT_INVALID;
        else error = write_bytes(path, manifest, PROJECT_MANIFEST_BYTES);
        if (error == APP_PROJECT_OK) error = verify_file(path, PROJECT_MANIFEST_BYTES, project_crc32(manifest, PROJECT_MANIFEST_BYTES));
    }
    if (error == APP_PROJECT_OK) error = sync_directory(generation);
    if (error == APP_PROJECT_OK) {
        char previous[19];
        const app_project_error_t old = pointer_read(base, "CURRENT", previous);
        if (old == APP_PROJECT_OK) error = pointer_write(base, "PREVIOUS", previous);
        else if (old != APP_PROJECT_NOT_FOUND && old != APP_PROJECT_CORRUPT) error = old;
    }
    if (error == APP_PROJECT_OK) error = pointer_write(base, "CURRENT", token);
    free(state); free(manifest); free(paths);
    return error;
}

static void catalog_entry(app_project_t *p, const char *name, bool inbox)
{
    app_project_snapshot_t *catalog = &p->catalog;
    if (inbox) {
        if (!project_filename_valid(name)) return;
        const char *dot = strrchr(name, '.');
        if (!dot) return;
        char extension[6] = {0};
        if (strlen(dot) >= sizeof(extension)) return;
        for (unsigned i = 0; dot[i]; ++i) extension[i] = (char)tolower((unsigned char)dot[i]);
        if (strcmp(extension, ".s3m") && strcmp(extension, ".wav")) return;
        char path[PROJECT_PATH_BYTES]; struct stat st;
        if (!project_path(path, p->inbox, name) || stat(path, &st) || (st.st_mode & S_IFMT) != S_IFREG || st.st_size <= 0) return;
        if (catalog->inbox_count == APP_PROJECT_LIST_MAX) { catalog->inbox_truncated = true; return; }
        app_project_inbox_entry_t *entry = &catalog->inbox[catalog->inbox_count++];
        strcpy(entry->filename, name); entry->bytes = (uint64_t)st.st_size;
        entry->kind = !strcmp(extension, ".s3m") ? APP_PROJECT_INBOX_MODULE : APP_PROJECT_INBOX_SAMPLE;
    } else {
        if (!project_id_valid(name)) return;
        if (catalog->entry_count == APP_PROJECT_LIST_MAX) { catalog->entries_truncated = true; return; }
        char base[PROJECT_PATH_BYTES], token[19];
        if (!project_path(base, p->projects, name)) return;
        app_project_error_t error = pointer_read(base, "CURRENT", token);
        project_payload_t *payload = calloc(1, sizeof(*payload));
        if (!payload) return;
        if (error == APP_PROJECT_OK) error = generation_load(p, name, token, payload, true);
        if (error == APP_PROJECT_OK) {
            app_project_entry_t *entry = &catalog->entries[catalog->entry_count++];
            strcpy(entry->id, name); strcpy(entry->name, payload->name);
            entry->generation = payload->generation; entry->saved_revision = payload->revision;
            entry->bytes = payload->module_size + payload->bank_size + PROJECT_STATE_BYTES + PROJECT_MANIFEST_BYTES;
        }
        project_payload_destroy(payload); free(payload);
    }
}

static app_project_error_t scan_directory(app_project_t *p, const char *path, bool inbox)
{
#if defined(_WIN32)
    char pattern[PROJECT_PATH_BYTES];
    if (!project_path(pattern, path, "*")) return APP_PROJECT_INVALID;
    WIN32_FIND_DATAA *entry = calloc(1, sizeof(*entry));
    if (!entry) return APP_PROJECT_NO_MEMORY;
    HANDLE scan = FindFirstFileA(pattern, entry);
    if (scan == INVALID_HANDLE_VALUE) { free(entry); return GetLastError() == ERROR_FILE_NOT_FOUND ? APP_PROJECT_OK : APP_PROJECT_IO_ERROR; }
    do { if (strcmp(entry->cFileName, ".") && strcmp(entry->cFileName, "..")) catalog_entry(p, entry->cFileName, inbox); }
    while (FindNextFileA(scan, entry));
    const DWORD error = GetLastError();
    FindClose(scan); free(entry);
    return error == ERROR_NO_MORE_FILES ? APP_PROJECT_OK : APP_PROJECT_IO_ERROR;
#else
    DIR *scan = opendir(path);
    if (!scan) return project_errno_error();
    struct dirent *entry;
    while ((entry = readdir(scan)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) catalog_entry(p, entry->d_name, inbox);
    }
    return closedir(scan) ? project_errno_error() : APP_PROJECT_OK;
#endif
}

app_project_error_t project_refresh(app_project_t *p)
{
    memset(&p->catalog, 0, sizeof(p->catalog));
    app_project_error_t error = scan_directory(p, p->projects, false);
    if (error == APP_PROJECT_OK) error = scan_directory(p, p->inbox, true);
#if defined(ESP_PLATFORM)
    uint64_t total = 0, used = 0;
    const bool usage_valid = app_nand_info(&total, &used) == ESP_OK;
#else
    /* A host path is a host filesystem; do not label disk-wide capacity as
     * onboard NAND space. Capacity remains explicitly unavailable here. */
    const uint64_t total = 0, used = 0;
    const bool usage_valid = false;
#endif
    project_platform_lock(&p->platform);
    p->snapshot.entry_count = p->catalog.entry_count;
    p->snapshot.inbox_count = p->catalog.inbox_count;
    p->snapshot.entries_truncated = p->catalog.entries_truncated;
    p->snapshot.inbox_truncated = p->catalog.inbox_truncated;
    memcpy(p->snapshot.entries, p->catalog.entries, sizeof(p->snapshot.entries));
    memcpy(p->snapshot.inbox, p->catalog.inbox, sizeof(p->snapshot.inbox));
    p->snapshot.storage_usage_valid = usage_valid;
    p->snapshot.storage_total_bytes = total; p->snapshot.storage_used_bytes = used;
    project_platform_unlock(&p->platform);
    return error;
}
