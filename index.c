#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>
#include <inttypes.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("    staged:    %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("    deleted:   %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("    modified:  %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0 || strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("    untracked: %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("    (nothing to show)\n");
    printf("\n");
    return 0;
}

int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode, size;
    unsigned long long mtime;
    char path[512];

    while (fscanf(f, "%o %64s %llu %u %511s\n", &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        IndexEntry *e = &index->entries[index->count++];
        e->mode = (uint32_t)mode;
        hex_to_hash(hex, &e->hash);
        e->mtime_sec = (uint64_t)mtime;
        e->size = (uint32_t)size;
        // Ensuring null-termination of path strings to prevent buffer overruns
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
    }
    fclose(f);
    return 0;
}

static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    // Use heap to avoid stack overflow when sorting
    IndexEntry *sorted_entries = malloc(sizeof(IndexEntry) * index->count);
    if (!sorted_entries) { fclose(f); return -1; }
    memcpy(sorted_entries, index->entries, sizeof(IndexEntry) * index->count);
    qsort(sorted_entries, (size_t)index->count, sizeof(IndexEntry), compare_index_entries);

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < index->count; i++) {
        IndexEntry *e = &sorted_entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %llu %u %s\n", e->mode, hex, (unsigned long long)e->mtime_sec, e->size, e->path);
    }
    free(sorted_entries);
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    ObjectID blob_id;
    object_write(OBJ_BLOB, data, size, &blob_id);
    free(data);

    struct stat st;
    lstat(path, &st);
    IndexEntry *entry = index_find(index, path);
    if (!entry) entry = &index->entries[index->count++];

    // Mapping filesystem permissions to internal VCS file modes (executable vs regular)
    entry->mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    entry->hash = blob_id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    strncpy(entry->path, path, sizeof(entry->path) - 1);
    entry->path[sizeof(entry->path) - 1] = '\0';

    return index_save(index);
}