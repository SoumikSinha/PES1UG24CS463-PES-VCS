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

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    // TODO: Implement recursive tree building
    // (See Lab Appendix for logical steps)
    (void)id_out;
    return -1;
}