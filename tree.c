// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char name[256];
} PendingFile;

typedef struct PendingTreeNode {
    char name[256];
    PendingFile *files;
    size_t file_count;
    size_t file_cap;
    struct PendingTreeNode *dirs;
    size_t dir_count;
    size_t dir_cap;
} PendingTreeNode;

static int load_index_snapshot(Index *index);
static PendingTreeNode *find_subdir(PendingTreeNode *node, const char *name);
static int ensure_dir_capacity(PendingTreeNode *node);
static int ensure_file_capacity(PendingTreeNode *node);
static PendingTreeNode *get_or_add_subdir(PendingTreeNode *node, const char *name);
static int add_file_entry(PendingTreeNode *node, const char *name, uint32_t mode, const ObjectID *hash);
static int insert_index_entry(PendingTreeNode *root, const IndexEntry *entry);
static void free_pending_tree(PendingTreeNode *node);
static int write_tree_recursive(PendingTreeNode *node, ObjectID *id_out);

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
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
    Index index;
    PendingTreeNode root;

    if (!id_out) return -1;

    if (load_index_snapshot(&index) != 0) return -1;

    memset(&root, 0, sizeof(root));
    root.name[0] = '\0';

    for (int i = 0; i < index.count; i++) {
        if (insert_index_entry(&root, &index.entries[i]) != 0) {
            free_pending_tree(&root);
            return -1;
        }
    }

    if (write_tree_recursive(&root, id_out) != 0) {
        free_pending_tree(&root);
        return -1;
    }

    free_pending_tree(&root);
    return 0;
}

static int compare_pending_dirs(const void *a, const void *b) {
    const PendingTreeNode *lhs = (const PendingTreeNode *)a;
    const PendingTreeNode *rhs = (const PendingTreeNode *)b;
    return strcmp(lhs->name, rhs->name);
}

static int load_index_snapshot(Index *index) {
    FILE *f;
    char line[2048];

    index->count = 0;

    f = fopen(INDEX_FILE, "r");
    if (!f) return errno == ENOENT ? 0 : -1;

    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned int mode;
        unsigned long long mtime_sec;
        unsigned int size;
        char hash_hex[HASH_HEX_SIZE + 1];
        char path[sizeof(index->entries[0].path)];
        IndexEntry *entry;

        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(f);
            return -1;
        }

        if (sscanf(line, "%o %64s %llu %u %511[^\n]", &mode, hash_hex, &mtime_sec, &size, path) != 5) {
            fclose(f);
            return -1;
        }

        entry = &index->entries[index->count];
        entry->mode = mode;
        entry->mtime_sec = (uint64_t)mtime_sec;
        entry->size = size;
        snprintf(entry->path, sizeof(entry->path), "%s", path);
        if (hex_to_hash(hash_hex, &entry->hash) != 0) {
            fclose(f);
            return -1;
        }

        index->count++;
    }

    if (ferror(f)) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

static PendingTreeNode *find_subdir(PendingTreeNode *node, const char *name) {
    for (size_t i = 0; i < node->dir_count; i++) {
        if (strcmp(node->dirs[i].name, name) == 0) return &node->dirs[i];
    }
    return NULL;
}

static int ensure_dir_capacity(PendingTreeNode *node) {
    PendingTreeNode *dirs;
    size_t new_cap;

    if (node->dir_count < node->dir_cap) return 0;

    new_cap = node->dir_cap == 0 ? 4 : node->dir_cap * 2;
    dirs = realloc(node->dirs, new_cap * sizeof(*dirs));
    if (!dirs) return -1;

    node->dirs = dirs;
    node->dir_cap = new_cap;
    return 0;
}

static int ensure_file_capacity(PendingTreeNode *node) {
    PendingFile *files;
    size_t new_cap;

    if (node->file_count < node->file_cap) return 0;

    new_cap = node->file_cap == 0 ? 4 : node->file_cap * 2;
    files = realloc(node->files, new_cap * sizeof(*files));
    if (!files) return -1;

    node->files = files;
    node->file_cap = new_cap;
    return 0;
}

static PendingTreeNode *get_or_add_subdir(PendingTreeNode *node, const char *name) {
    PendingTreeNode *dir = find_subdir(node, name);
    if (dir) return dir;

    for (size_t i = 0; i < node->file_count; i++) {
        if (strcmp(node->files[i].name, name) == 0) return NULL;
    }

    if (ensure_dir_capacity(node) != 0) return NULL;

    dir = &node->dirs[node->dir_count++];
    memset(dir, 0, sizeof(*dir));
    snprintf(dir->name, sizeof(dir->name), "%s", name);
    return dir;
}

static int add_file_entry(PendingTreeNode *node, const char *name, uint32_t mode, const ObjectID *hash) {
    PendingFile *file;

    if (find_subdir(node, name) != NULL) return -1;

    for (size_t i = 0; i < node->file_count; i++) {
        if (strcmp(node->files[i].name, name) == 0) {
            node->files[i].mode = mode;
            node->files[i].hash = *hash;
            return 0;
        }
    }

    if (ensure_file_capacity(node) != 0) return -1;

    file = &node->files[node->file_count++];
    file->mode = mode;
    file->hash = *hash;
    snprintf(file->name, sizeof(file->name), "%s", name);
    return 0;
}

static int insert_index_entry(PendingTreeNode *root, const IndexEntry *entry) {
    PendingTreeNode *node = root;
    const char *component = entry->path;

    if (entry->path[0] == '\0') return -1;

    while (1) {
        const char *slash = strchr(component, '/');
        char name[256];
        size_t name_len = slash ? (size_t)(slash - component) : strlen(component);

        if (name_len == 0 || name_len >= sizeof(name)) return -1;
        memcpy(name, component, name_len);
        name[name_len] = '\0';

        if (!slash) return add_file_entry(node, name, entry->mode, &entry->hash);

        node = get_or_add_subdir(node, name);
        if (!node) return -1;
        component = slash + 1;
        if (*component == '\0') return -1;
    }
}

static void free_pending_tree(PendingTreeNode *node) {
    for (size_t i = 0; i < node->dir_count; i++) {
        free_pending_tree(&node->dirs[i]);
    }

    free(node->dirs);
    free(node->files);
}

static int write_tree_recursive(PendingTreeNode *node, ObjectID *id_out) {
    Tree tree;
    void *data = NULL;
    size_t len = 0;
    int rc;

    memset(&tree, 0, sizeof(tree));

    qsort(node->dirs, node->dir_count, sizeof(*node->dirs), compare_pending_dirs);

    if (node->file_count + node->dir_count > MAX_TREE_ENTRIES) return -1;

    for (size_t i = 0; i < node->file_count; i++) {
        TreeEntry *entry = &tree.entries[tree.count++];
        entry->mode = node->files[i].mode;
        entry->hash = node->files[i].hash;
        snprintf(entry->name, sizeof(entry->name), "%s", node->files[i].name);
    }

    for (size_t i = 0; i < node->dir_count; i++) {
        ObjectID child_id;
        TreeEntry *entry;

        if (write_tree_recursive(&node->dirs[i], &child_id) != 0) {
            free(data);
            return -1;
        }

        entry = &tree.entries[tree.count++];
        entry->mode = MODE_DIR;
        entry->hash = child_id;
        snprintf(entry->name, sizeof(entry->name), "%s", node->dirs[i].name);
    }

    if (tree.count == 0) {
        return object_write(OBJ_TREE, "", 0, id_out);
    }

    rc = tree_serialize(&tree, &data, &len);
    if (rc != 0) return -1;

    rc = object_write(OBJ_TREE, data, len, id_out);
    free(data);
    return rc;
}
