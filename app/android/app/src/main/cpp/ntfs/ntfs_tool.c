/*
 * droiddesk-ntfs — files on an NTFS volume without FUSE or a kernel driver, on top of
 * libntfs-3g (tuxera/ntfs-3g, GPL-2.0-or-later). droiddesk-usb runs it with libdroiddesk_blk.so,
 * so DEVICE is /dev/droiddesk-blk (the held USB disk); it works on image files as well.
 *
 *   droiddesk-ntfs [--ro] DEVICE < commands
 *
 * Built as libdroiddesk_ntfs.so (the APK only carries shared libraries): droiddesk-usb calls
 * droiddesk_ntfs_main() through Python ctypes; -DNTFS_TOOL_EXECUTABLE adds main() for tests.
 *
 * Same protocol as droiddesk-exfat. One command per line, fields separated by tabs; one
 * mount for the whole batch:
 *   ls PATH           -> "L\t<d|f>\t<size>\t<name>" per entry
 *   tree PATH         -> "T\t<d|f>\t<size>\t<path below PATH>" for everything below, parents first
 *   stat PATH         -> "S\t<d|f>\t<size>"
 *   put LOCAL PATH    copy a local file to PATH (replaced if it exists)
 *   get PATH LOCAL    copy PATH to a local file
 *   mkdir PATH        create a folder (fine if it is already there)
 *   rm PATH           delete a file
 *   rmdir PATH        delete an empty folder
 * Errors: "E\t<command>\t<message>" on stdout; the batch goes on, the exit code is 1.
 * Progress: "P\t<bytes>" after every chunk that put/get moved.
 * A volume that cannot be mounted gives "E\tmount\t<message>" and exit code 1.
 *
 * Writing is refused (the volume is not even opened read-write) when Windows hibernated or
 * kept the volume in its fast-startup cache, or when it was not cleanly closed; reading works.
 * Metafiles ($MFT, $LogFile, ...) and other "$" entries of the root folder are not listed;
 * MS-DOS 8.3 aliases are not listed. New names must be valid on Windows.
 */
#include "config.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "types.h"
#include "attrib.h"
#include "dir.h"
#include "inode.h"
#include "layout.h"
#include "logging.h"
#include "ntfstime.h"
#include "unistr.h"
#include "volume.h"

#define CHUNK (1 << 20)

static ntfs_volume *vol;
static int failures;
static char buffer[CHUNK];

static void fail(const char *command, const char *path, int rc) {
    failures++;
    printf("E\t%s\t%s: %s\n", command, path, strerror(rc < 0 ? -rc : rc));
}

static int err(void) { return errno ? errno : EIO; }

static int is_dir(const ntfs_inode *ni) { return (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) != 0; }

/* Size of the unnamed data stream (what Windows shows as the file size). */
static unsigned long long file_size(ntfs_inode *ni) {
    if (is_dir(ni)) return 0;
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) return 0;
    unsigned long long size = (unsigned long long)na->data_size;
    ntfs_attr_close(na);
    return size;
}

static ntfs_inode *lookup(const char *path) { return ntfs_pathname_to_inode(vol, NULL, path); }

/* Splits PATH into its parent folder (opened) and the last component as NTFS UTF-16. */
static ntfs_inode *open_parent(const char *path, ntfschar **uname, int *ulen) {
    char *copy = strdup(path);
    size_t n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/') copy[--n] = 0;
    char *slash = strrchr(copy, '/');
    const char *base = slash ? slash + 1 : copy;
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        free(copy);
        errno = EINVAL;
        return NULL;
    }
    *uname = NULL;
    *ulen = ntfs_mbstoucs(base, uname);
    if (*ulen < 0) {
        free(copy);
        errno = EILSEQ;
        return NULL;
    }
    if (*ulen > NTFS_MAX_NAME_LEN) {
        free(*uname);
        free(copy);
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (slash == copy) copy[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(copy, "/");
    ntfs_inode *dir = lookup(copy);
    int saved = errno;
    free(copy);
    if (!dir) {
        free(*uname);
        errno = saved;
        return NULL;
    }
    if (!is_dir(dir)) {
        ntfs_inode_close(dir);
        free(*uname);
        errno = ENOTDIR;
        return NULL;
    }
    return dir;
}

struct entries {
    size_t count, cap;
    int root;
    char **names;
    MFT_REF *refs;
    int *dirs;
};

static int collect(void *data, const ntfschar *name, const int len, const int type, const s64 pos,
                   const MFT_REF mref, const unsigned dt_type) {
    (void)pos;
    struct entries *e = data;
    if (type == FILE_NAME_DOS) return 0;
    char *utf8 = NULL;
    if (ntfs_ucstombs(name, len, &utf8, 0) < 0 || !utf8) return 0;
    if (strcmp(utf8, ".") == 0 || strcmp(utf8, "..") == 0 ||
        (e->root && (utf8[0] == '$' || MREF(mref) < FILE_first_user))) {
        free(utf8);
        return 0;
    }
    if (e->count == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 16;
        e->names = realloc(e->names, e->cap * sizeof *e->names);
        e->refs = realloc(e->refs, e->cap * sizeof *e->refs);
        e->dirs = realloc(e->dirs, e->cap * sizeof *e->dirs);
    }
    e->names[e->count] = utf8;
    e->refs[e->count] = mref;
    e->dirs[e->count++] = dt_type == NTFS_DT_DIR;
    return 0;
}

static void list(const char *path, const char *prefix, int recursive) {
    const char *command = recursive ? "tree" : "ls";
    ntfs_inode *dir = lookup(path);
    if (!dir) {
        fail(command, path, err());
        return;
    }
    if (!is_dir(dir)) {
        ntfs_inode_close(dir);
        fail(command, path, ENOTDIR);
        return;
    }
    /* Children first collected, so no inode is opened inside the index walk. */
    struct entries e = {0};
    e.root = dir->mft_no == FILE_root;
    s64 pos = 0;
    int rc = ntfs_readdir(dir, &pos, &e, collect) ? err() : 0;
    ntfs_inode_close(dir);
    if (rc) fail(command, path, rc);
    for (size_t i = 0; i < e.count; i++) {
        unsigned long long size = 0;
        if (!e.dirs[i]) {
            ntfs_inode *ni = ntfs_inode_open(vol, MREF(e.refs[i]));
            if (ni) {
                size = file_size(ni);
                ntfs_inode_close(ni);
            }
        }
        if (recursive) printf("T\t%c\t%llu\t%s%s\n", e.dirs[i] ? 'd' : 'f', size, prefix, e.names[i]);
        else printf("L\t%c\t%llu\t%s\n", e.dirs[i] ? 'd' : 'f', size, e.names[i]);
    }
    for (size_t i = 0; i < e.count; i++) {
        if (recursive && e.dirs[i]) {
            char *child = malloc(strlen(path) + strlen(e.names[i]) + 2);
            char *below = malloc(strlen(prefix) + strlen(e.names[i]) + 2);
            sprintf(child, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e.names[i]);
            sprintf(below, "%s%s/", prefix, e.names[i]);
            list(child, below, 1);
            free(child);
            free(below);
        }
        free(e.names[i]);
    }
    free(e.names);
    free(e.refs);
    free(e.dirs);
}

static void stat_path(const char *path) {
    ntfs_inode *ni = lookup(path);
    if (!ni) {
        fail("stat", path, err());
        return;
    }
    printf("S\t%c\t%llu\n", is_dir(ni) ? 'd' : 'f', file_size(ni));
    ntfs_inode_close(ni);
}

/* A new entry named UNAME in DIR; names Windows could not open are refused. */
static ntfs_inode *create(ntfs_inode *dir, ntfschar *uname, int ulen, mode_t type) {
    if (ntfs_forbidden_names(vol, uname, ulen, TRUE)) {
        errno = EINVAL;
        return NULL;
    }
    return ntfs_create(dir, const_cpu_to_le32(0), uname, (u8)ulen, type);
}

/*
 * PATH if it exists, else an entry of its folder whose name differs only in case (new names
 * are POSIX-namespace, matched case-sensitively, but Windows would see both as one name),
 * else a new TYPE entry. NULL with errno on failure.
 */
static ntfs_inode *find_or_create(const char *path, mode_t type) {
    ntfs_inode *ni = lookup(path);
    if (ni || errno != ENOENT) return ni;
    ntfschar *uname;
    int ulen, rc = 0;
    ntfs_inode *dir = open_parent(path, &uname, &ulen);
    if (!dir) return NULL;
    NVolClearCaseSensitive(vol);
    u64 mref = ntfs_inode_lookup_by_name(dir, uname, ulen);
    NVolSetCaseSensitive(vol);
    if (mref != (u64)-1) ni = ntfs_inode_open(vol, MREF(mref));
    else ni = create(dir, uname, ulen, type);
    if (!ni) rc = err();
    free(uname);
    if (ntfs_inode_close(dir) && !rc) rc = err();
    if (rc && ni) {
        ntfs_inode_close(ni);
        ni = NULL;
    }
    errno = rc;
    return ni;
}

/* "P\t<bytes>" after every chunk of put/get: droiddesk-usb shows the progress of a batch. */
static void progress(long long bytes) {
    printf("P\t%lld\n", bytes);
    fflush(stdout);
}

static void put(const char *local, const char *path) {
    int fd = open(local, O_RDONLY);
    if (fd < 0) {
        fail("put", local, errno);
        return;
    }
    struct stat st;
    fstat(fd, &st);
    int rc = 0;
    ntfs_inode *ni = find_or_create(path, S_IFREG);
    if (!ni) rc = err();
    else if (is_dir(ni)) {
        ntfs_inode_close(ni);
        close(fd);
        fail("put", path, EISDIR);
        return;
    }
    ntfs_attr *na = NULL;
    if (!rc) {
        na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
        if (!na || ntfs_attr_truncate(na, 0)) rc = err();
    }
    s64 offset = 0;
    while (!rc) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0) {
            rc = errno;
            break;
        }
        if (n == 0) break;
        for (ssize_t done = 0; done < n;) {
            s64 written = ntfs_attr_pwrite(na, offset + done, n - done, buffer + done);
            if (written <= 0) {
                rc = written < 0 ? err() : ENOSPC;
                break;
            }
            done += written;
        }
        offset += n;
        progress(n);
    }
    close(fd);
    if (na) ntfs_attr_close(na);
    if (ni) {
        if (!rc) {
            ni->last_access_time = timespec2ntfs(st.st_atim);
            ni->last_data_change_time = timespec2ntfs(st.st_mtim);
            ni->last_mft_change_time = ntfs_current_time();
            ntfs_inode_mark_dirty(ni);
            NInoFileNameSetDirty(ni);
        }
        if (ntfs_inode_close(ni) && !rc) rc = err();
    }
    if (rc) fail("put", path, rc);
}

static void get(const char *path, const char *local) {
    ntfs_inode *ni = lookup(path);
    if (!ni) {
        fail("get", path, err());
        return;
    }
    if (is_dir(ni)) {
        ntfs_inode_close(ni);
        fail("get", path, EISDIR);
        return;
    }
    ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
    if (!na) {
        int rc = err();
        ntfs_inode_close(ni);
        fail("get", path, rc);
        return;
    }
    int fd = open(local, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        int rc = errno;
        ntfs_attr_close(na);
        ntfs_inode_close(ni);
        fail("get", local, rc);
        return;
    }
    int rc = 0;
    s64 offset = 0;
    while (offset < na->data_size) {
        s64 want = na->data_size - offset < (s64)sizeof buffer ? na->data_size - offset : (s64)sizeof buffer;
        s64 n = ntfs_attr_pread(na, offset, want, buffer);
        if (n <= 0) {
            rc = n < 0 ? err() : EIO;
            break;
        }
        for (s64 done = 0; done < n;) {
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
    struct timespec times[2] = {ntfs2timespec(ni->last_access_time), ntfs2timespec(ni->last_data_change_time)};
    futimens(fd, times);
    if (close(fd) != 0 && rc == 0) rc = errno;
    ntfs_attr_close(na);
    ntfs_inode_close(ni);
    if (rc != 0) fail("get", path, rc);
}

static void make_dir(const char *path) {
    ntfs_inode *ni = find_or_create(path, S_IFDIR);
    if (!ni) {
        fail("mkdir", path, err());
        return;
    }
    int dir = is_dir(ni);
    int rc = ntfs_inode_close(ni) ? err() : 0;
    if (!dir) rc = EEXIST;
    if (rc) fail("mkdir", path, rc);
}

static void remove_path(const char *path, int want_dir) {
    const char *command = want_dir ? "rmdir" : "rm";
    ntfs_inode *ni = lookup(path);
    if (!ni) {
        fail(command, path, err());
        return;
    }
    if (ni->mft_no < FILE_first_user) {
        ntfs_inode_close(ni);
        fail(command, path, EPERM);
        return;
    }
    if (is_dir(ni) != want_dir) {
        ntfs_inode_close(ni);
        fail(command, path, want_dir ? ENOTDIR : EISDIR);
        return;
    }
    ntfschar *uname;
    int ulen;
    ntfs_inode *dir = open_parent(path, &uname, &ulen);
    if (!dir) {
        int rc = err();
        ntfs_inode_close(ni);
        fail(command, path, rc);
        return;
    }
    /* ntfs_delete() closes ni in any case. */
    int rc = ntfs_delete(vol, path, ni, dir, uname, (u8)ulen) ? err() : 0;
    free(uname);
    if (ntfs_inode_close(dir) && !rc) rc = err();
    if (rc) fail(command, path, rc);
}

static void mount_failed(const char *device, int rc) {
    failures++;
    if (rc == EPERM)
        printf("E\tmount\tNTFS je označený jako používaný (Windows hibernace/rychlé spuštění) — zápis odmítnut\n");
    else if (rc == EOPNOTSUPP)
        printf("E\tmount\tNTFS nebyl ve Windows řádně odpojen — zápis odmítnut (připojte ho ve Windows a bezpečně odeberte)\n");
    else if (rc == EUCLEAN)
        printf("E\tmount\tNTFS je označený ke kontrole (chkdsk) — zápis odmítnut\n");
    else
        printf("E\tmount\t%s: %s\n", device, strerror(rc));
}

int droiddesk_ntfs_main(int argc, char **argv) {
    int ro = argc > 1 && strcmp(argv[1], "--ro") == 0;
    if (argc != 2 + ro) {
        fprintf(stderr, "usage: droiddesk-ntfs [--ro] DEVICE < commands\n");
        return 2;
    }
    const char *device = argv[1 + ro];
    failures = 0;
    ntfs_log_set_handler(ntfs_log_handler_stderr);
    ntfs_log_clear_levels(~0U);
    ntfs_log_set_levels(NTFS_LOG_LEVEL_CRITICAL);
    /* Read-write without NTFS_MNT_RECOVER: a hibernated or fast-startup volume fails with
     * EPERM, an unclean $LogFile with EOPNOTSUPP; nothing is reset or replayed. */
    vol = ntfs_mount(device, ro ? NTFS_MNT_RDONLY : NTFS_MNT_NONE);
    if (!vol) {
        int rc = err();
        if (!ro && rc == EOPNOTSUPP) {
            /* The $LogFile check if a read-only mount works, else really unsupported. */
            ntfs_volume *probe = ntfs_mount(device, NTFS_MNT_RDONLY);
            if (probe) ntfs_umount(probe, FALSE);
            else rc = err();
        }
        mount_failed(device, rc);
        fflush(stdout);
        return 1;
    }
    if (!ro && (vol->flags & VOLUME_IS_DIRTY)) {
        ntfs_umount(vol, FALSE);
        mount_failed(device, EUCLEAN);
        fflush(stdout);
        return 1;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, stdin)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        char *command = strtok(line, "\t");
        char *first = strtok(NULL, "\t");
        char *second = strtok(NULL, "\t");
        if (!command || !first) continue;
        if (strcmp(command, "ls") == 0) list(first, "", 0);
        else if (strcmp(command, "tree") == 0) list(first, "", 1);
        else if (strcmp(command, "stat") == 0) stat_path(first);
        else if (strcmp(command, "put") == 0 && second && !ro) put(first, second);
        else if (strcmp(command, "get") == 0 && second) get(first, second);
        else if (strcmp(command, "mkdir") == 0 && !ro) make_dir(first);
        else if (strcmp(command, "rm") == 0 && !ro) remove_path(first, 0);
        else if (strcmp(command, "rmdir") == 0 && !ro) remove_path(first, 1);
        else {
            failures++;
            printf("E\t%s\tunknown command or read-only\n", command);
        }
        fflush(stdout);
    }
    free(line);
    /* Writes back every inode, the MFT mirror and the bitmaps, then fsync()s and closes. */
    if (ntfs_umount(vol, FALSE)) {
        failures++;
        printf("E\tumount\t%s: %s\n", device, strerror(err()));
    }
    vol = NULL;
    fflush(stdout);
    return failures ? 1 : 0;
}

#ifdef NTFS_TOOL_EXECUTABLE
int main(int argc, char **argv) { return droiddesk_ntfs_main(argc, argv); }
#endif
