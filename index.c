// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/resource.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

static int compare_index_entries(const void *a, const void *b) {
    const IndexEntry *lhs = (const IndexEntry *)a;
    const IndexEntry *rhs = (const IndexEntry *)b;
    return strcmp(lhs->path, rhs->path);
}

static uint32_t index_mode_from_stat(const struct stat *st) {
    return (st->st_mode & S_IXUSR) ? 0100755 : 0100644;
}

__attribute__((constructor))
static void ensure_stack_limit(void) {
    struct rlimit rl;
    const rlim_t desired = 16 * 1024 * 1024;

    if (getrlimit(RLIMIT_STACK, &rl) != 0) return;
    if (rl.rlim_cur >= desired) return;

    if (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= desired) {
        rl.rlim_cur = desired;
        (void)setrlimit(RLIMIT_STACK, &rl);
    } else if (rl.rlim_max > rl.rlim_cur) {
        rl.rlim_cur = rl.rlim_max;
        (void)setrlimit(RLIMIT_STACK, &rl);
    }
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
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

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    FILE *f;
    char line[2048];

    if (!index) return -1;

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

// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    IndexEntry *sorted_entries = NULL;
    FILE *f = NULL;
    int fd = -1;
    int dir_fd = -1;
    int rc = -1;
    const char *temp_path = ".pes/index.tmp";

    if (!index) return -1;
    if (index->count < 0 || index->count > MAX_INDEX_ENTRIES) return -1;

    if (index->count > 0) {
        sorted_entries = malloc((size_t)index->count * sizeof(*sorted_entries));
        if (!sorted_entries) return -1;
        memcpy(sorted_entries, index->entries, (size_t)index->count * sizeof(*sorted_entries));
        qsort(sorted_entries, index->count, sizeof(IndexEntry), compare_index_entries);
    }

    f = fopen(temp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < index->count; i++) {
        char hash_hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted_entries[i].hash, hash_hex);
        if (fprintf(f, "%o %s %llu %u %s\n",
                    sorted_entries[i].mode,
                    hash_hex,
                    (unsigned long long)sorted_entries[i].mtime_sec,
                    sorted_entries[i].size,
                    sorted_entries[i].path) < 0) {
            goto cleanup;
        }
    }

    if (fflush(f) != 0) goto cleanup;
    fd = fileno(f);
    if (fd < 0 || fsync(fd) != 0) goto cleanup;
    if (fclose(f) != 0) {
        f = NULL;
        goto cleanup;
    }
    f = NULL;

    if (rename(temp_path, INDEX_FILE) != 0) goto cleanup;

    dir_fd = open(PES_DIR, O_RDONLY | O_DIRECTORY);
    if (dir_fd >= 0) {
        if (fsync(dir_fd) != 0) goto cleanup;
    }

    rc = 0;

cleanup:
    if (dir_fd >= 0) close(dir_fd);
    if (f) fclose(f);
    if (rc != 0) unlink(temp_path);
    free(sorted_entries);
    return rc;
}

// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    struct stat st;
    FILE *f = NULL;
    void *data = NULL;
    size_t read_len = 0;
    ObjectID id;
    IndexEntry *entry;

    if (!index || !path) return -1;
    if (index->count < 0 || index->count > MAX_INDEX_ENTRIES) return -1;

    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;

    f = fopen(path, "rb");
    if (!f) return -1;

    if (st.st_size > 0) {
        data = malloc((size_t)st.st_size);
        if (!data) goto cleanup;
        read_len = fread(data, 1, (size_t)st.st_size, f);
        if (read_len != (size_t)st.st_size) goto cleanup;
    } else {
        data = malloc(1);
        if (!data) goto cleanup;
    }

    if (fclose(f) != 0) {
        f = NULL;
        goto cleanup;
    }
    f = NULL;

    if (object_write(OBJ_BLOB, data, (size_t)st.st_size, &id) != 0) goto cleanup;

    entry = index_find(index, path);
    if (!entry) {
        if (index->count >= MAX_INDEX_ENTRIES) goto cleanup;
        entry = &index->entries[index->count++];
    }

    entry->mode = index_mode_from_stat(&st);
    entry->hash = id;
    entry->mtime_sec = (uint64_t)st.st_mtime;
    entry->size = (uint32_t)st.st_size;
    snprintf(entry->path, sizeof(entry->path), "%s", path);

    free(data);
    return index_save(index);

cleanup:
    if (f) fclose(f);
    free(data);
    return -1;
}
