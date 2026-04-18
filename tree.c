#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;
    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;
        char mode_str[16] = {0};
        memcpy(mode_str, ptr, space - ptr);
        entry->mode = (uint32_t)strtol(mode_str, NULL, 8);
        ptr = space + 1;
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        memcpy(entry->name, ptr, null_byte - ptr);
        entry->name[null_byte - ptr] = '\0';
        ptr = null_byte + 1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;
        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    Tree sorted = *tree;
    qsort(sorted.entries, (size_t)sorted.count, sizeof(TreeEntry), compare_tree_entries);
    size_t offset = 0;
    for (int i = 0; i < sorted.count; i++) {
        const TreeEntry *entry = &sorted.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += (size_t)written + 1;
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }
    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

static int write_tree_for_range(IndexEntry *entries, int start, int end, const char *prefix, ObjectID *id_out) {
    Tree tree; tree.count = 0;
    size_t plen = strlen(prefix);
    int i = start;
    while (i < end) {
        const char *rel = (plen == 0) ? entries[i].path : entries[i].path + plen + 1;
        const char *slash = strchr(rel, '/');
        if (!slash) {
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = entries[i].mode; te->hash = entries[i].hash;
            strncpy(te->name, rel, 255); i++;
        } else {
            char dir_name[256]; size_t dir_len = slash - rel;
            strncpy(dir_name, rel, dir_len); dir_name[dir_len] = '\0';
            char sub_prefix[512];
            if (plen == 0) sprintf(sub_prefix, "%s", dir_name);
            else sprintf(sub_prefix, "%s/%s", prefix, dir_name);
            int j = i;
            while (j < end && strncmp(entries[j].path, sub_prefix, strlen(sub_prefix)) == 0) j++;
            ObjectID sub_id;
            write_tree_for_range(entries, i, j, sub_prefix, &sub_id);
            TreeEntry *te = &tree.entries[tree.count++];
            te->mode = MODE_DIR; te->hash = sub_id; strncpy(te->name, dir_name, 255);
            i = j;
        }
    }
    void *data; size_t len;
    tree_serialize(&tree, &data, &len);
    object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return 0;
}

int tree_from_index(ObjectID *id_out) {
    Index *index = malloc(sizeof(Index));
    if (!index || index_load(index) != 0) {
        if (index) free(index);
    return -1;
    }
    int rc = write_tree_for_range(index->entries, 0, index->count, "", id_out);
    free(index);
    return rc;
}