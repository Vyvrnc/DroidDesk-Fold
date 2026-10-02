/*
 * libdroiddesk_blk.so — LD_PRELOAD shim that turns /dev/droiddesk-blk into the USB disk
 * DroidDesk holds (UsbBridge request "blk"), so unmodified tools (mkfs.fat, mkfs.exfat,
 * mkfs.ext4, debugfs, mtools) work on it without root or a kernel block device.
 *
 * Environment (set by droiddesk-usb):
 *   DROIDDESK_BLK_DEVICE  USB device name (/dev/bus/usb/001/002), required
 *   DROIDDESK_BLK_LUN     card reader slot, default "-" (the only inserted card)
 *   DROIDDESK_BLK_OFFSET  start of the visible range in bytes (a partition), default 0
 *   DROIDDESK_BLK_SIZE    length of the visible range in bytes, default up to the end
 *
 * The path looks like a regular file of that size: no block ioctls, no discard. All
 * opens in a process share one bridge session; the last close flushes the device cache
 * (SYNCHRONIZE CACHE). Other paths and file descriptors pass through untouched.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define VPATH "/dev/droiddesk-blk"
#define SOCKET_NAME "droiddesk.usb"
#define MAX_FDS 32
#define MAX_REQUEST (1 << 20) /* the bridge's limit per request */
#define CACHE_BYTES (128 * 1024)
#define MAX_BLOCK 4096

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int conn = -1;
static int refs;
static uint32_t bs;
static uint64_t base; /* bytes from the start of the device */
static uint64_t size; /* visible bytes, a multiple of bs */
static unsigned char cache[CACHE_BYTES];
static uint64_t cache_off = UINT64_MAX;
static size_t cache_len;
static unsigned char scratch[MAX_BLOCK];
static struct {
    int fd;
    uint64_t pos;
} fds[MAX_FDS];
static int nfds;

#define REAL(ret, name, ...)                                              \
    static ret (*real_##name)(__VA_ARGS__);                               \
    static void load_##name(void) {                                       \
        if (!real_##name) real_##name = dlsym(RTLD_NEXT, #name);          \
    }

REAL(int, open, const char *, int, ...)
REAL(int, openat, int, const char *, int, ...)
REAL(int, close, int)
REAL(ssize_t, read, int, void *, size_t)
REAL(ssize_t, write, int, const void *, size_t)
REAL(ssize_t, pread64, int, void *, size_t, off64_t)
REAL(ssize_t, pwrite64, int, const void *, size_t, off64_t)
REAL(off64_t, lseek64, int, off64_t, int)
REAL(int, fstat, int, struct stat *)
REAL(int, fstatat, int, const char *, struct stat *, int)
REAL(int, stat, const char *, struct stat *)
REAL(int, lstat, const char *, struct stat *)
REAL(int, access, const char *, int)
REAL(int, faccessat, int, const char *, int, int)
REAL(int, fsync, int)
REAL(int, fdatasync, int)
REAL(int, ftruncate64, int, off64_t)
REAL(int, fallocate64, int, int, off64_t, off64_t)
REAL(int, posix_fadvise64, int, off64_t, off64_t, int)
REAL(int, flock, int, int)
REAL(char *, realpath, const char *, char *)

static int is_vpath(const char *path) { return path && strcmp(path, VPATH) == 0; }

static int slot_of(int fd) {
    for (int i = 0; i < nfds; i++)
        if (fds[i].fd == fd) return i;
    return -1;
}

/* Lock-free peek for the pass-through fast path; only a hit takes the lock. */
static int tracked(int fd) {
    if (fd < 0 || nfds == 0) return 0;
    pthread_mutex_lock(&lock);
    int hit = slot_of(fd) >= 0;
    pthread_mutex_unlock(&lock);
    return hit;
}

static void complain(const char *what) { fprintf(stderr, "droiddesk-blk: %s\n", what); }

/* DROIDDESK_BLK_DEBUG=1: every call on the device to stderr. */
static int debug = -1;
static void trace(const char *format, ...) {
    if (debug < 0) debug = getenv("DROIDDESK_BLK_DEBUG") != NULL;
    if (!debug) return;
    va_list ap;
    va_start(ap, format);
    fprintf(stderr, "droiddesk-blk: ");
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int send_all(const void *data, size_t len) {
    const unsigned char *p = data;
    while (len) {
        ssize_t n = send(conn, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(void *data, size_t len) {
    unsigned char *p = data;
    while (len) {
        ssize_t n = recv(conn, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_line(char *line, size_t cap) {
    size_t used = 0;
    for (;;) {
        char c;
        if (recv_all(&c, 1) < 0) return -1;
        if (c == '\n') break;
        if (used + 1 < cap) line[used++] = c;
    }
    line[used] = 0;
    return 0;
}

static uint64_t env_u64(const char *name, uint64_t fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    return strtoull(value, NULL, 10);
}

/* Opens the bridge session ("blk"); called with the lock held. */
static int connect_bridge(void) {
    const char *device = getenv("DROIDDESK_BLK_DEVICE");
    const char *lun = getenv("DROIDDESK_BLK_LUN");
    if (!device || !*device) {
        complain("DROIDDESK_BLK_DEVICE is not set (use droiddesk-usb)");
        errno = ENODEV;
        return -1;
    }
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, SOCKET_NAME, strlen(SOCKET_NAME));
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(SOCKET_NAME));
    if (connect(s, (struct sockaddr *)&addr, len) < 0) {
        complain("DroidDesk does not answer (is the Linux session running?)");
        load_close();
        real_close(s);
        errno = ENODEV;
        return -1;
    }
    conn = s;
    char request[512];
    int n = snprintf(request, sizeof request, "blk %s %s\n", device, lun && *lun ? lun : "-");
    char line[512];
    if (n <= 0 || (size_t)n >= sizeof request || send_all(request, (size_t)n) < 0) goto fail;
    for (;;) {
        if (recv_line(line, sizeof line) < 0) goto fail;
        if (strcmp(line, "permission") == 0) {
            complain("confirm the USB permission on the phone's screen");
            continue;
        }
        break;
    }
    unsigned long long capacity = 0;
    unsigned int block = 0;
    if (sscanf(line, "device %llu %u", &capacity, &block) != 2 || block == 0 || block > MAX_BLOCK ||
        CACHE_BYTES % block != 0) {
        complain(strncmp(line, "err ", 4) == 0 ? line + 4 : line);
        goto fail;
    }
    bs = block;
    base = env_u64("DROIDDESK_BLK_OFFSET", 0);
    if (base % bs != 0 || base >= capacity) {
        complain("DROIDDESK_BLK_OFFSET is outside the device or not block aligned");
        goto fail;
    }
    size = env_u64("DROIDDESK_BLK_SIZE", capacity - base);
    if (size > capacity - base) size = capacity - base;
    size -= size % bs;
    cache_off = UINT64_MAX;
    return 0;
fail:
    load_close();
    real_close(s);
    conn = -1;
    errno = EIO;
    return -1;
}

static void put_be(unsigned char *p, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) {
        p[i] = (unsigned char)value;
        value >>= 8;
    }
}

/* One request; count blocks from the device's own block address lba. */
static int request(char op, uint64_t lba, uint32_t count, void *data) {
    unsigned char header[13];
    header[0] = (unsigned char)op;
    put_be(header + 1, lba, 8);
    put_be(header + 9, count, 4);
    size_t bytes = (size_t)count * bs;
    if (send_all(header, sizeof header) < 0) goto broken;
    if (op == 'W' && send_all(data, bytes) < 0) goto broken;
    unsigned char status;
    if (recv_all(&status, 1) < 0) goto broken;
    if (status != 0) {
        unsigned char len[2];
        char message[512];
        if (recv_all(len, 2) < 0) goto broken;
        size_t n = ((size_t)len[0] << 8) | len[1];
        size_t keep = n < sizeof message - 1 ? n : sizeof message - 1;
        if (recv_all(message, keep) < 0) goto broken;
        for (size_t rest = n - keep; rest;) {
            char skip[64];
            size_t part = rest < sizeof skip ? rest : sizeof skip;
            if (recv_all(skip, part) < 0) goto broken;
            rest -= part;
        }
        message[keep] = 0;
        complain(message);
        errno = EIO;
        return -1;
    }
    if (op == 'R' && recv_all(data, bytes) < 0) goto broken;
    return 0;
broken:
    complain("connection to DroidDesk lost");
    errno = EIO;
    return -1;
}

static void cache_update(uint64_t off, const unsigned char *data, size_t len) {
    if (cache_off == UINT64_MAX) return;
    uint64_t start = off > cache_off ? off : cache_off;
    uint64_t end = off + len < cache_off + cache_len ? off + len : cache_off + cache_len;
    if (start < end) memcpy(cache + (start - cache_off), data + (start - off), (size_t)(end - start));
}

static ssize_t do_pread(void *buf, size_t len, uint64_t off) {
    trace("read %zu @ %llu", len, (unsigned long long)off);
    if (off >= size) return 0;
    if (len > size - off) len = (size_t)(size - off);
    size_t done = 0;
    while (done < len) {
        uint64_t at = off + done;
        if (cache_off == UINT64_MAX || at < cache_off || at >= cache_off + cache_len) {
            uint64_t start = at / bs * bs;
            size_t n = CACHE_BYTES;
            if (start + n > size) n = (size_t)(size - start);
            if (request('R', (base + start) / bs, (uint32_t)(n / bs), cache) < 0) {
                cache_off = UINT64_MAX;
                return done ? (ssize_t)done : -1;
            }
            cache_off = start;
            cache_len = n;
        }
        size_t avail = (size_t)(cache_off + cache_len - at);
        size_t want = len - done < avail ? len - done : avail;
        memcpy((unsigned char *)buf + done, cache + (at - cache_off), want);
        done += want;
    }
    return (ssize_t)done;
}

static ssize_t do_pwrite(const void *buf, size_t len, uint64_t off) {
    trace("write %zu @ %llu", len, (unsigned long long)off);
    if (len == 0) return 0;
    if (off >= size) {
        errno = ENOSPC;
        return -1;
    }
    if (len > size - off) len = (size_t)(size - off);
    const unsigned char *src = buf;
    size_t done = 0;
    while (done < len) {
        uint64_t at = off + done;
        size_t inside = (size_t)(at % bs);
        if (inside == 0 && len - done >= bs) {
            size_t n = (len - done) / bs * bs;
            if (n > MAX_REQUEST) n = MAX_REQUEST;
            if (request('W', (base + at) / bs, (uint32_t)(n / bs), (void *)(src + done)) < 0)
                return done ? (ssize_t)done : -1;
            cache_update(at, src + done, n);
            done += n;
        } else {
            /* Part of a block: read, modify, write it back. */
            uint64_t start = at - inside;
            if (do_pread(scratch, bs, start) != (ssize_t)bs) return done ? (ssize_t)done : -1;
            size_t n = bs - inside < len - done ? bs - inside : len - done;
            memcpy(scratch + inside, src + done, n);
            if (request('W', (base + start) / bs, 1, scratch) < 0) return done ? (ssize_t)done : -1;
            cache_update(start, scratch, bs);
            done += n;
        }
    }
    return (ssize_t)done;
}

static int do_flush(void) { return conn >= 0 ? request('F', 0, 0, NULL) : 0; }

/* Ends the session; called with the lock held. Q has no reply. */
static void disconnect(void) {
    unsigned char quit[13] = {'Q'};
    send_all(quit, sizeof quit);
    load_close();
    real_close(conn);
    conn = -1;
    cache_off = UINT64_MAX;
}

static int vopen(int flags) {
    pthread_mutex_lock(&lock);
    if (nfds >= MAX_FDS) {
        pthread_mutex_unlock(&lock);
        errno = EMFILE;
        return -1;
    }
    if (conn < 0 && connect_bridge() < 0) {
        int saved = errno;
        pthread_mutex_unlock(&lock);
        errno = saved;
        return -1;
    }
    /* A real descriptor of its own (the session socket), so the kernel never reuses the number. */
    int fd = fcntl(conn, (flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 3);
    if (fd >= 0) {
        fds[nfds].fd = fd;
        fds[nfds].pos = 0;
        nfds++;
        refs++;
    }
    trace("open flags 0x%x -> fd %d, size %llu", flags, fd, (unsigned long long)size);
    pthread_mutex_unlock(&lock);
    return fd;
}

static void fill_stat(struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0666;
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_size = (off_t)size;
    st->st_blksize = 65536;
    st->st_blocks = (blkcnt_t)(size / 512);
    st->st_dev = 0xdd;
    st->st_ino = 0xdd;
}

/* stat() on the path before any open: the bridge has to tell the size. */
static int vstat(struct stat *st) {
    pthread_mutex_lock(&lock);
    int ok = conn >= 0 || connect_bridge() == 0;
    if (ok) fill_stat(st);
    /* A session opened only for stat() is closed again right away. */
    if (ok && refs == 0) disconnect();
    trace("stat -> %s, size %llu", ok ? "ok" : "failed", (unsigned long long)size);
    pthread_mutex_unlock(&lock);
    return ok ? 0 : -1;
}

/* ── open/close ── */

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (is_vpath(path)) return vopen(flags);
    load_open();
    return real_open(path, flags, mode);
}

int open64(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return open(path, flags, mode);
}

int __open_2(const char *path, int flags) { return open(path, flags, 0); }

int __open64_2(const char *path, int flags) { return open(path, flags, 0); }

int openat(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    if (is_vpath(path)) return vopen(flags);
    load_openat();
    return real_openat(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openat(dirfd, path, flags, mode);
}

int __openat_2(int dirfd, const char *path, int flags) { return openat(dirfd, path, flags, 0); }

int __openat64_2(int dirfd, const char *path, int flags) { return openat(dirfd, path, flags, 0); }

int close(int fd) {
    load_close();
    if (!tracked(fd)) return real_close(fd);
    pthread_mutex_lock(&lock);
    int i = slot_of(fd);
    int result = 0;
    if (i >= 0) {
        fds[i] = fds[--nfds];
        real_close(fd);
        if (--refs == 0 && conn >= 0) {
            if (do_flush() < 0) result = -1;
            disconnect();
        }
        trace("close %d (open: %d)", fd, refs);
    }
    pthread_mutex_unlock(&lock);
    return result;
}

/* ── data ── */

static ssize_t vread(int fd, void *buf, size_t len) {
    pthread_mutex_lock(&lock);
    int i = slot_of(fd);
    ssize_t n = do_pread(buf, len, fds[i].pos);
    if (n > 0) fds[i].pos += (uint64_t)n;
    pthread_mutex_unlock(&lock);
    return n;
}

static ssize_t vwrite(int fd, const void *buf, size_t len) {
    pthread_mutex_lock(&lock);
    int i = slot_of(fd);
    ssize_t n = do_pwrite(buf, len, fds[i].pos);
    if (n > 0) fds[i].pos += (uint64_t)n;
    pthread_mutex_unlock(&lock);
    return n;
}

static ssize_t vpread(void *buf, size_t len, off64_t off) {
    if (off < 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&lock);
    ssize_t n = do_pread(buf, len, (uint64_t)off);
    pthread_mutex_unlock(&lock);
    return n;
}

static ssize_t vpwrite(const void *buf, size_t len, off64_t off) {
    if (off < 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&lock);
    ssize_t n = do_pwrite(buf, len, (uint64_t)off);
    pthread_mutex_unlock(&lock);
    return n;
}

ssize_t read(int fd, void *buf, size_t len) {
    if (tracked(fd)) return vread(fd, buf, len);
    load_read();
    return real_read(fd, buf, len);
}

ssize_t __read_chk(int fd, void *buf, size_t len, size_t buflen) {
    (void)buflen;
    return read(fd, buf, len);
}

ssize_t write(int fd, const void *buf, size_t len) {
    if (tracked(fd)) return vwrite(fd, buf, len);
    load_write();
    return real_write(fd, buf, len);
}

ssize_t __write_chk(int fd, const void *buf, size_t len, size_t buflen) {
    (void)buflen;
    return write(fd, buf, len);
}

ssize_t pread64(int fd, void *buf, size_t len, off64_t off) {
    if (tracked(fd)) return vpread(buf, len, off);
    load_pread64();
    return real_pread64(fd, buf, len, off);
}

ssize_t pread(int fd, void *buf, size_t len, off_t off) { return pread64(fd, buf, len, (off64_t)off); }

ssize_t __pread_chk(int fd, void *buf, size_t len, off_t off, size_t buflen) {
    (void)buflen;
    return pread64(fd, buf, len, (off64_t)off);
}

ssize_t __pread64_chk(int fd, void *buf, size_t len, off64_t off, size_t buflen) {
    (void)buflen;
    return pread64(fd, buf, len, off);
}

ssize_t pwrite64(int fd, const void *buf, size_t len, off64_t off) {
    if (tracked(fd)) return vpwrite(buf, len, off);
    load_pwrite64();
    return real_pwrite64(fd, buf, len, off);
}

ssize_t pwrite(int fd, const void *buf, size_t len, off_t off) { return pwrite64(fd, buf, len, (off64_t)off); }

ssize_t __pwrite_chk(int fd, const void *buf, size_t len, off_t off, size_t buflen) {
    (void)buflen;
    return pwrite64(fd, buf, len, (off64_t)off);
}

ssize_t __pwrite64_chk(int fd, const void *buf, size_t len, off64_t off, size_t buflen) {
    (void)buflen;
    return pwrite64(fd, buf, len, off);
}

off64_t lseek64(int fd, off64_t off, int whence) {
    load_lseek64();
    if (!tracked(fd)) return real_lseek64(fd, off, whence);
    pthread_mutex_lock(&lock);
    int i = slot_of(fd);
    int64_t from = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)fds[i].pos
                 : whence == SEEK_END ? (int64_t)size : -1;
    off64_t result = -1;
    if (from < 0 || from + off < 0) {
        errno = EINVAL;
    } else {
        fds[i].pos = (uint64_t)(from + off);
        result = (off64_t)fds[i].pos;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

off_t lseek(int fd, off_t off, int whence) { return (off_t)lseek64(fd, (off64_t)off, whence); }

int fsync(int fd) {
    load_fsync();
    if (!tracked(fd)) return real_fsync(fd);
    pthread_mutex_lock(&lock);
    int result = do_flush();
    pthread_mutex_unlock(&lock);
    return result;
}

int fdatasync(int fd) {
    load_fdatasync();
    if (!tracked(fd)) return real_fdatasync(fd);
    return fsync(fd);
}

/* ── metadata ── */

int fstat(int fd, struct stat *st) {
    load_fstat();
    if (!tracked(fd)) return real_fstat(fd, st);
    pthread_mutex_lock(&lock);
    fill_stat(st);
    pthread_mutex_unlock(&lock);
    return 0;
}

int fstat64(int fd, struct stat64 *st) { return fstat(fd, (struct stat *)st); }

int fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    load_fstatat();
    if (is_vpath(path)) return vstat(st);
    if (path && !*path && (flags & AT_EMPTY_PATH) && tracked(dirfd)) return fstat(dirfd, st);
    return real_fstatat(dirfd, path, st, flags);
}

int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags) {
    return fstatat(dirfd, path, (struct stat *)st, flags);
}

int stat(const char *path, struct stat *st) {
    load_stat();
    if (is_vpath(path)) return vstat(st);
    return real_stat(path, st);
}

int stat64(const char *path, struct stat64 *st) { return stat(path, (struct stat *)st); }

int lstat(const char *path, struct stat *st) {
    load_lstat();
    if (is_vpath(path)) return vstat(st);
    return real_lstat(path, st);
}

int lstat64(const char *path, struct stat64 *st) { return lstat(path, (struct stat *)st); }

int access(const char *path, int mode) {
    load_access();
    if (is_vpath(path)) return 0;
    return real_access(path, mode);
}

int faccessat(int dirfd, const char *path, int mode, int flags) {
    load_faccessat();
    if (is_vpath(path)) return 0;
    return real_faccessat(dirfd, path, mode, flags);
}

char *realpath(const char *path, char *resolved) {
    load_realpath();
    if (!is_vpath(path)) return real_realpath(path, resolved);
    char *out = resolved ? resolved : malloc(PATH_MAX);
    if (out) strcpy(out, VPATH);
    return out;
}

char *__realpath_chk(const char *path, char *resolved, size_t resolvedlen) {
    (void)resolvedlen;
    return realpath(path, resolved);
}

int ftruncate64(int fd, off64_t length) {
    load_ftruncate64();
    if (!tracked(fd)) return real_ftruncate64(fd, length);
    if (length >= 0 && (uint64_t)length <= size) return 0;
    errno = EFBIG;
    return -1;
}

int ftruncate(int fd, off_t length) { return ftruncate64(fd, (off64_t)length); }

/* No discard: tools would then assume zeroed blocks that still hold old data. */
int fallocate64(int fd, int mode, off64_t off, off64_t len) {
    load_fallocate64();
    if (!tracked(fd)) return real_fallocate64(fd, mode, off, len);
    errno = EOPNOTSUPP;
    return -1;
}

int fallocate(int fd, int mode, off_t off, off_t len) { return fallocate64(fd, mode, (off64_t)off, (off64_t)len); }

int posix_fallocate64(int fd, off64_t off, off64_t len) {
    if (!tracked(fd)) {
        static int (*real)(int, off64_t, off64_t);
        if (!real) real = dlsym(RTLD_NEXT, "posix_fallocate64");
        return real(fd, off, len);
    }
    return off >= 0 && len >= 0 && (uint64_t)(off + len) <= size ? 0 : EFBIG;
}

int posix_fallocate(int fd, off_t off, off_t len) {
    if (!tracked(fd)) {
        static int (*real)(int, off_t, off_t);
        if (!real) real = dlsym(RTLD_NEXT, "posix_fallocate");
        return real(fd, off, len);
    }
    return posix_fallocate64(fd, (off64_t)off, (off64_t)len);
}

int posix_fadvise64(int fd, off64_t off, off64_t len, int advice) {
    load_posix_fadvise64();
    if (!tracked(fd)) return real_posix_fadvise64(fd, off, len, advice);
    return 0;
}

int posix_fadvise(int fd, off_t off, off_t len, int advice) {
    return posix_fadvise64(fd, (off64_t)off, (off64_t)len, advice);
}

int flock(int fd, int operation) {
    load_flock();
    if (!tracked(fd)) return real_flock(fd, operation);
    return 0;
}

/* A regular file: block device ioctls (size, sector size, discard) are not there. */
#ifdef __BIONIC__
int ioctl(int fd, int request, ...)
#else
int ioctl(int fd, unsigned long request, ...)
#endif
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (tracked(fd)) {
        trace("ioctl 0x%lx -> ENOTTY", (unsigned long)request);
        errno = ENOTTY;
        return -1;
    }
#ifdef __BIONIC__
    static int (*real)(int, int, ...);
#else
    static int (*real)(int, unsigned long, ...);
#endif
    if (!real) real = dlsym(RTLD_NEXT, "ioctl");
    return real(fd, request, arg);
}
