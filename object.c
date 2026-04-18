// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTATION ──────────────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//
// Steps:
//   1. Build the full object: header + data
//   2. Compute SHA-256 hash of the FULL object
//   3. Check for deduplication
//   4. Create shard directory
//   5. Write to temp file
//   6. fsync + rename (atomic)
//   7. fsync directory
//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    // 1. Build header string
    const char *type_str;
    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    char header[64];
    int hlen = snprintf(header, sizeof(header), "%s %zu", type_str, len);

    // Full object = header + '\0' + data
    size_t total = (size_t)hlen + 1 + len;
    uint8_t *full = malloc(total);
    if (!full) return -1;

    memcpy(full, header, hlen);
    full[hlen] = '\0';
    memcpy(full + hlen + 1, data, len);

    // 2. Compute hash of full object
    ObjectID id;
    compute_hash(full, total, &id);

    // 3. Deduplication: if already stored, just return the hash
    if (object_exists(&id)) {
        *id_out = id;
        free(full);
        return 0;
    }

    // 4. Build shard path and create shard directory
    char path[512];
    object_path(&id, path, sizeof(path));

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) { free(full); return -1; }
    *slash = '\0';
    mkdir(dir, 0755); // OK if it already exists

    // 5. Write to a temporary file in the shard directory
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", path);
    // Use a deterministic temp name to avoid needing mkstemp portability issues
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0444);
    if (fd < 0) { free(full); return -1; }

    ssize_t written = write(fd, full, total);
    free(full);
    if (written != (ssize_t)total) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }

    // 6. fsync the temp file to ensure data reaches disk
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmp_path);
        return -1;
    }
    close(fd);

    // 7. Atomically rename temp file to final path
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
    return -1;
    }

    // 8. fsync the shard directory to persist the rename
    int dir_fd = open(dir, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
    }

    // 9. Return the computed hash
    *id_out = id;
    return 0;
}

// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    // 1. Get path
    char path[512];
    object_path(id, path, sizeof(path));

    // 2. Open and read the entire file
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)file_size + 1);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    // 3. Verify integrity: recompute hash and compare to id
    ObjectID computed;
    compute_hash(buf, (size_t)file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        fprintf(stderr, "error: object integrity check failed\n");
        free(buf);
    return -1;
    }

    // 4. Parse header: find the '\0' separator
    uint8_t *null_byte = memchr(buf, '\0', (size_t)file_size);
    if (!null_byte) { free(buf); return -1; }

    char *header = (char *)buf;

    if (strncmp(header, "blob ", 5) == 0)         *type_out = OBJ_BLOB;
    else if (strncmp(header, "tree ", 5) == 0)    *type_out = OBJ_TREE;
    else if (strncmp(header, "commit ", 7) == 0)  *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // 5. Extract data (after the null byte)
    size_t data_offset = (size_t)(null_byte - buf) + 1;
    size_t data_len    = (size_t)file_size - data_offset;

    uint8_t *data = malloc(data_len + 1);
    if (!data) { free(buf); return -1; }
    memcpy(data, buf + data_offset, data_len);
    data[data_len] = '\0'; // null-terminate for convenience

    free(buf);
    *data_out = data;
    *len_out  = data_len;
    return 0;
}

// Final check for Phase 1 completion.