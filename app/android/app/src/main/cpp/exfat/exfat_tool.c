/*
 * droiddesk-exfat — files on an exFAT volume without FUSE or a kernel driver, on top of
 * libexfat (relan/exfat, GPL-2.0-or-later). droiddesk-usb runs it with libdroiddesk_blk.so,
 * so DEVICE is /dev/droiddesk-blk (the held USB disk); it works on image files as well.
 *
 *   droiddesk-exfat [--ro] DEVICE < commands
 *
 * Built as libdroiddesk_exfat.so (the APK only carries shared libraries): droiddesk-usb calls
 * droiddesk_exfat_main() through Python ctypes; -DEXFAT_TOOL_EXECUTABLE adds main() for tests.
 *
 * One command per line, fields separated by tabs; one mount for the whole batch:
 *   ls PATH           -> "L\t<d|f>\t<size>\t<name>" per entry
 *   tree PATH         -> "T\t<d|f>\t<size>\t<path below PATH>" for everything below, parents first
 *   stat PATH         -> "S\t<d|f>\t<size>"
 *   put LOCAL PATH    copy a local file to PATH (replaced if it exists)
 *   get PATH LOCAL    copy PATH to a local file
 *   mkdir PATH        create a folder (fine if it is already there)
 *   rm PATH           delete a file
 *   rmdir PATH        delete an empty folder
 *   mv PATH NEWPATH   rename/move a file or folder; an existing NEWPATH is replaced like
 *                     rename(2) does (a file by a file, an empty folder by a folder)
 * Errors: "E\t<command>\t<message>" on stdout; the batch goes on, the exit code is 1.
 * Progress: "P\t<bytes>" after every chunk that put/get moved.
 */
#include "exfat.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHUNK (1 << 20)

static struct exfat ef;
static int failures;
static char buffer[CHUNK];

static void fail(const char *command, const char *path, int rc) {
    failures++;
    printf("E\t%s\t%s: %s\n", command, path, strerror(rc < 0 ? -rc : rc));
}

/* "P\t<bytes>" after every chunk of put/get: droiddesk-usb shows the progress of a batch. */
static void progress(long long bytes) {
    printf("P\t%lld\n", bytes);
    fflush(stdout);
}

static int is_dir(const struct exfat_node *node) { return (node->attrib & EXFAT_ATTRIB_DIR) != 0; }

static void list(const char *path, const char *prefix, int recursive) {
    struct exfat_node *dir;
    int rc = exfat_lookup(&ef, &dir, path);
    if (rc != 0) {
        fail(recursive ? "tree" : "ls", path, rc);
        return;
    }
    if (!is_dir(dir)) {
        exfat_put_node(&ef, dir);
        fail(recursive ? "tree" : "ls", path, -ENOTDIR);
        return;
    }
    struct exfat_iterator it;
    rc = exfat_opendir(&ef, dir, &it);
    if (rc != 0) {
        exfat_put_node(&ef, dir);
        fail(recursive ? "tree" : "ls", path, rc);
        return;
    }
    /* Children first collected, so the recursion does not run inside an open iterator. */
    size_t count = 0, cap = 16;
    char **names = malloc(cap * sizeof *names);
    int *dirs = malloc(cap * sizeof *dirs);
    struct exfat_node *node;
    while ((node = exfat_readdir(&it))) {
        char name[EXFAT_UTF8_NAME_BUFFER_MAX];
        exfat_get_name(node, name);
        if (recursive)
            printf("T\t%c\t%llu\t%s%s\n", is_dir(node) ? 'd' : 'f', (unsigned long long)node->size, prefix, name);
        else
            printf("L\t%c\t%llu\t%s\n", is_dir(node) ? 'd' : 'f', (unsigned long long)node->size, name);
        if (count == cap) {
            cap *= 2;
            names = realloc(names, cap * sizeof *names);
            dirs = realloc(dirs, cap * sizeof *dirs);
        }
        names[count] = strdup(name);
        dirs[count++] = is_dir(node);
        exfat_put_node(&ef, node);
    }
    exfat_closedir(&ef, &it);
    exfat_put_node(&ef, dir);
    for (size_t i = 0; i < count; i++) {
        if (recursive && dirs[i]) {
            char *child = malloc(strlen(path) + strlen(names[i]) + 2);
            char *below = malloc(strlen(prefix) + strlen(names[i]) + 2);
            sprintf(child, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", names[i]);
            sprintf(below, "%s%s/", prefix, names[i]);
            list(child, below, 1);
            free(child);
            free(below);
        }
        free(names[i]);
    }
    free(names);
    free(dirs);
}

static void stat_path(const char *path) {
    struct exfat_node *node;
    int rc = exfat_lookup(&ef, &node, path);
    if (rc != 0) {
        fail("stat", path, rc);
        return;
    }
    printf("S\t%c\t%llu\n", is_dir(node) ? 'd' : 'f', (unsigned long long)node->size);
    exfat_put_node(&ef, node);
}

static void put(const char *local, const char *path) {
    int fd = open(local, O_RDONLY);
    if (fd < 0) {
        fail("put", local, errno);
        return;
    }
    struct stat st;
    fstat(fd, &st);
    struct exfat_node *node;
    int rc = exfat_lookup(&ef, &node, path);
    if (rc == 0) {
        if (is_dir(node)) {
            exfat_put_node(&ef, node);
            close(fd);
            fail("put", path, -EISDIR);
            return;
        }
        rc = exfat_truncate(&ef, node, 0, true);
    } else {
        rc = exfat_mknod(&ef, path);
        if (rc == 0) rc = exfat_lookup(&ef, &node, path);
        else node = NULL;
    }
    if (rc != 0) {
        if (node) exfat_put_node(&ef, node);
        close(fd);
        fail("put", path, rc);
        return;
    }
    off_t offset = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0) {
            rc = errno;
            break;
        }
        if (n == 0) break;
        ssize_t written = exfat_generic_pwrite(&ef, node, buffer, (size_t)n, offset);
        if (written != n) {
            rc = written < 0 ? (int)-written : ENOSPC;
            break;
        }
        offset += n;
        progress(n);
    }
    close(fd);
    if (rc == 0) {
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        exfat_utimes(node, times);
    }
    int flushed = exfat_flush_node(&ef, node);
    exfat_put_node(&ef, node);
    if (rc == 0) rc = flushed;
    if (rc != 0) fail("put", path, rc);
}

static void get(const char *path, const char *local) {
    struct exfat_node *node;
    int rc = exfat_lookup(&ef, &node, path);
    if (rc != 0) {
        fail("get", path, rc);
        return;
    }
    if (is_dir(node)) {
        exfat_put_node(&ef, node);
        fail("get", path, -EISDIR);
        return;
    }
    int fd = open(local, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        exfat_put_node(&ef, node);
        fail("get", local, errno);
        return;
    }
    off_t offset = 0;
    while ((uint64_t)offset < node->size) {
        size_t want = node->size - (uint64_t)offset < sizeof buffer ? (size_t)(node->size - (uint64_t)offset) : sizeof buffer;
        ssize_t n = exfat_generic_pread(&ef, node, buffer, want, offset);
        if (n <= 0) {
            rc = n < 0 ? (int)-n : EIO;
            break;
        }
        for (ssize_t done = 0; done < n;) {
            ssize_t w = write(fd, buffer + done, (size_t)(n - done));
            if (w < 0) {
                rc = errno;
                break;
            }
            done += w;
        }
        if (rc) break;
        offset += n;
        progress(n);
    }
    struct timespec times[2] = {{node->atime, 0}, {node->mtime, 0}};
    futimens(fd, times);
    if (close(fd) != 0 && rc == 0) rc = errno;
    exfat_put_node(&ef, node);
    if (rc != 0) fail("get", path, rc);
}

static void make_dir(const char *path) {
    struct exfat_node *node;
    if (exfat_lookup(&ef, &node, path) == 0) {
        int dir = is_dir(node);
        exfat_put_node(&ef, node);
        if (!dir) fail("mkdir", path, -EEXIST);
        return;
    }
    int rc = exfat_mkdir(&ef, path);
    if (rc != 0) fail("mkdir", path, rc);
}

static void remove_path(const char *path, int dir) {
    struct exfat_node *node;
    int rc = exfat_lookup(&ef, &node, path);
    if (rc != 0) {
        fail(dir ? "rmdir" : "rm", path, rc);
        return;
    }
    rc = dir ? exfat_rmdir(&ef, node) : exfat_unlink(&ef, node);
    exfat_put_node(&ef, node);
    /* An unlinked node frees its clusters only in cleanup, also when the parent's flush
     * failed: otherwise they stay allocated without a directory entry. */
    if (node->is_unlinked) {
        int cleaned = exfat_cleanup_node(&ef, node);
        if (rc == 0) rc = cleaned;
    }
    if (rc != 0) fail(dir ? "rmdir" : "rm", path, rc);
}

int droiddesk_exfat_main(int argc, char **argv) {
    int ro = argc > 1 && strcmp(argv[1], "--ro") == 0;
    if (argc != 2 + ro) {
        fprintf(stderr, "usage: droiddesk-exfat [--ro] DEVICE < commands\n");
        return 2;
    }
    if (exfat_mount(&ef, argv[1 + ro], ro ? "ro" : "noatime") != 0) return 1;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, stdin)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        char *command = strtok(line, "\t");
        char *first = strtok(NULL, "\t");
        char *second = strtok(NULL, "\t");
        char *extra = strtok(NULL, "\t");
        if (!command || !first) continue;
        /* put, get and mv take two paths, everything else one: a tab inside a name must not
         * shift the fields onto another file. */
        int two = strcmp(command, "put") == 0 || strcmp(command, "get") == 0 || strcmp(command, "mv") == 0;
        if (extra || (two != (second != NULL))) {
            failures++;
            printf("E\t%s\twrong number of fields\n", command);
            fflush(stdout);
            continue;
        }
        if (strcmp(command, "ls") == 0) list(first, "", 0);
        else if (strcmp(command, "tree") == 0) list(first, "", 1);
        else if (strcmp(command, "stat") == 0) stat_path(first);
        else if (strcmp(command, "put") == 0 && second && !ro) put(first, second);
        else if (strcmp(command, "get") == 0 && second) get(first, second);
        else if (strcmp(command, "mkdir") == 0 && !ro) make_dir(first);
        else if (strcmp(command, "rm") == 0 && !ro) remove_path(first, 0);
        else if (strcmp(command, "rmdir") == 0 && !ro) remove_path(first, 1);
        else if (strcmp(command, "mv") == 0 && second && !ro) {
            /* exfat_rename: replaces an existing target (and frees its clusters), refuses a
             * folder into itself; a change of case only is the same node. */
            int rc = exfat_rename(&ef, first, second);
            if (rc != 0) fail("mv", first, rc);
        }
        else {
            failures++;
            printf("E\t%s\tunknown command or read-only\n", command);
        }
        fflush(stdout);
    }
    free(line);
    exfat_unmount(&ef);
    fflush(stdout);
    return failures ? 1 : 0;
}

#ifdef EXFAT_TOOL_EXECUTABLE
int main(int argc, char **argv) { return droiddesk_exfat_main(argc, argv); }
#endif
