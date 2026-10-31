// Native Debian path and process dispatch. Only interposable libc calls are covered.
// Build with glibc, never the Android NDK. See docs/glibc-native-plan.md.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <elf.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <pwd.h>
#include <grp.h>
#include <pthread.h>
#include <ftw.h>
#include <signal.h>
#include <time.h>
#include <utime.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <sys/xattr.h>
extern char **environ;

/* The hook's own system calls skip the syscall() interposition below: it maps paths, and
 * the resolver's probes of physical rootfs paths would map themselves again, recursively. */
static long raw_syscall(long nr, ...) {
    static long (*real)(long, ...);
    long (*fn)(long, ...) = __atomic_load_n(&real, __ATOMIC_ACQUIRE);
    if (!fn) { fn = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall"); __atomic_store_n(&real, fn, __ATOMIC_RELEASE); }
    va_list ap;
    va_start(ap, nr);
    long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
    long d = va_arg(ap, long), e = va_arg(ap, long), f = va_arg(ap, long);
    va_end(ap);
    return fn(nr, a, b, c, d, e, f);
}

/* The program this process really runs. Started as "ld.so --library-path ... --preload ...
 * --argv0 NAME PROGRAM ARGS", /proc/self/exe is the loader; programs that re-execute
 * themselves (bun runs its embedded ripgrep as /proc/self/exe with argv0 "rg", so Claude
 * Code's Grep and Glob found nothing, Tab S10 FE) must get PROGRAM instead. Read once from
 * /proc/self/cmdline, which keeps the loader's original command line. */
static char self_exe[PATH_MAX];
static int ld_option_with_value(const char *a) {
    return !strcmp(a, "--library-path") || !strcmp(a, "--preload") || !strcmp(a, "--argv0") ||
           !strcmp(a, "--audit") || !strcmp(a, "--inhibit-rpath") || !strcmp(a, "--glibc-hwcaps-prepend") ||
           !strcmp(a, "--glibc-hwcaps-mask");
}
static int ld_option(const char *a) {
    return ld_option_with_value(a) || !strcmp(a, "--inhibit-cache") || !strcmp(a, "--list") ||
           !strcmp(a, "--verify") || !strcmp(a, "--help") || !strcmp(a, "--version") ||
           !strcmp(a, "--list-tunables") || !strcmp(a, "--list-diagnostics");
}
static int is_loader(const char *path) {
    const char *plib = getenv("DD_PLIB");
    char want[PATH_MAX];
    if (!plib || !path || snprintf(want, sizeof want, "%s/ld.so", plib) >= (int)sizeof want) return 0;
    return !strcmp(path, want);
}
__attribute__((constructor)) static void find_self_exe(void) {
    int fd = (int)raw_syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[8192];
    ssize_t n = 0, r;
    while (n < (ssize_t)sizeof buf - 1 && (r = read(fd, buf + n, sizeof buf - 1 - (size_t)n)) > 0) n += r;
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *a = buf, *end = buf + n, *argv0 = NULL;
    if (!is_loader(a)) return;
    for (a += strlen(a) + 1; a < end; a += strlen(a) + 1) {
        if (ld_option_with_value(a)) {
            if (!strcmp(a, "--argv0") && a + strlen(a) + 1 < end) argv0 = a + strlen(a) + 1;
            a += strlen(a) + 1; continue;
        }
        if (!strcmp(a, "--")) { a += strlen(a) + 1; break; }
        if (ld_option(a)) continue;
        break;
    }
    if (a < end && *a && strlen(a) < sizeof self_exe) strcpy(self_exe, a);
    /* Every native process was called "ld.so": pgrep, pkill and top showed only the loader. */
    const char *name = argv0 && *argv0 ? argv0 : self_exe;
    if (*name) {
        const char *base = strrchr(name, '/');
        prctl(PR_SET_NAME, base && base[1] ? base + 1 : name, 0, 0, 0);
    }
}
/* Programs from an app mirror (patchelf copies, Electron) can lose the native runtime from
 * their environment: Electron re-launches itself with a cleaned one, and its D-Bus client then
 * reached the Termux bus (keyring dialog). The mirror keeps it in .dd-env next to the program;
 * restore it before anything else runs when DD_ROOTFS is missing. The Termux session bus is
 * replaced by Debian's, other present values are kept. */
__attribute__((constructor(101))) static void restore_mirror_env(void) {
    if (getenv("DD_ROOTFS")) {
        /* Native Debian programs use Debian's session bus (its keyring), never Termux's. */
        const char *bus = getenv("DBUS_SESSION_BUS_ADDRESS");
        if (bus && strstr(bus, "dbus-session")) setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/tmp/dbus-debian", 1);
        return;
    }
    char path[PATH_MAX];
    ssize_t n = (ssize_t)raw_syscall(SYS_readlinkat, AT_FDCWD, "/proc/self/exe", path, sizeof path - 16);
    if (n <= 0) return;
    path[n] = 0;
    char *slash = strrchr(path, '/');
    if (!slash) return;
    strcpy(slash + 1, ".dd-env");
    int fd = (int)raw_syscall(SYS_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[4096];
    ssize_t len = 0, r;
    while (len < (ssize_t)sizeof buf - 1 && (r = read(fd, buf + len, sizeof buf - 1 - (size_t)len)) > 0) len += r;
    close(fd);
    if (len <= 0) return;
    buf[len] = 0;
    for (char *line = buf, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *eq = strchr(line, '=');
        if (!eq || eq == line) continue;
        *eq = 0;
        const char *current = getenv(line);
        int replace = !strcmp(line, "DBUS_SESSION_BUS_ADDRESS") && (!current || strstr(current, "dbus-session"));
        setenv(line, eq + 1, replace);
    }
}
/* The patched libc reads resolv.conf from fd 63, which the launcher opens. Bun closes every
 * fd above 2 when it spawns, so the Bash tool of native Claude had no DNS (Tab S10 FE).
 * Reopen it here; inheritable like the launcher's, so later children keep it. */
__attribute__((constructor(102))) static void restore_resolv_fd(void) {
    const char *root = getenv("DD_ROOTFS");
    char path[PATH_MAX];
    int e = errno;
    if (root && fcntl(63, F_GETFD) < 0 && errno == EBADF &&
        snprintf(path, sizeof path, "%s/etc/resolv.conf", root) < (int)sizeof path) {
        int fd = (int)raw_syscall(SYS_openat, AT_FDCWD, path, O_RDONLY);
        if (fd >= 0 && fd != 63) { dup2(fd, 63); close(fd); }
    }
    errno = e;
}
static int is_proc_self_exe(const char *p) {
    if (!p) return 0;
    if (!strcmp(p, "/proc/self/exe") || !strcmp(p, "/proc/thread-self/exe")) return 1;
    char mine[64];
    snprintf(mine, sizeof mine, "/proc/%d/exe", (int)getpid());
    return !strcmp(p, mine);
}
static ssize_t copy_self_exe(char *b, size_t n) {
    size_t len = strlen(self_exe);
    if (len > n) len = n;
    memcpy(b, self_exe, len);
    return (ssize_t)len;
}

/* Each invocation owns its buffers; nested calls and two-path operations cannot
 * overwrite another mapping. Symbol caches use atomic publication. */
static int pre(const char *p, const char *d) {
    size_t n = strlen(d);
    return !strncmp(p, d, n) && (p[n] == '/' || !p[n]);
}
static int system_path(const char *p) {
    const char *dirs[] = {"/usr", "/etc", "/var", "/opt", "/lib", "/bin", "/sbin", NULL};
    for (int i = 0; dirs[i]; ++i) if (pre(p, dirs[i])) return 1;
    return 0;
}
/* "/", "/.", "//", "/..": the root itself. */
static int root_only(const char *p) {
    for (const char *s = p; *s;) {
        while (*s == '/') ++s;
        if (!*s) break;
        size_t n = strcspn(s, "/");
        if (!(n == 1 && s[0] == '.') && !(n == 2 && s[0] == '.' && s[1] == '.')) return 0;
        s += n;
    }
    return 1;
}
/* Caller-owned stack buffers are private to each thread and nested invocation. */
static const char *mapped(const char *p, char b[PATH_MAX], int nofollow) {
    if (!p || p[0] != '/') return p;
    const char *root = getenv("DD_ROOTFS"), *tmp = getenv("DD_TMP");
    if (!root || (tmp && pre(p, tmp))) return p;
    /* The root itself is Debian's: ls / and find / listed Android's root, so native Claude's
     * find / found nothing in Debian, and cd / broke relative paths. Paths below / are mapped
     * as before, so /proc, /dev and /storage stay Android's. */
    if (root_only(p)) {
        if (snprintf(b, PATH_MAX, "%s", root) >= PATH_MAX) goto toolong;
        return b;
    }
    if (tmp && (pre(p, "/tmp") || pre(p, "/dev/shm"))) {
        if (snprintf(b, PATH_MAX, "%s%s", tmp, p + (p[1] == 't' ? 4 : 8)) >= PATH_MAX)
            goto toolong;
        return b;
    }
    /* Physical rootfs paths occur in HOME and in the initial exec command. */
    if (pre(p, root)) p += strlen(root);
    else if (!system_path(p) && !pre(p, "/root") && !pre(p, "/home")) return p;
    char pending[PATH_MAX], target[PATH_MAX], next[PATH_MAX];
    size_t floor = strlen(root), used = floor;
    if (floor >= PATH_MAX || strlen(p) >= PATH_MAX) goto toolong;
    strcpy(b, root); strcpy(pending, p);
    unsigned links = 0;
    while (*pending) {
        char *part = pending;
        while (*part == '/') ++part;
        if (!*part) break;
        char *end = strchr(part, '/');
        size_t n = end ? (size_t)(end - part) : strlen(part);
        const char *rest = end ? end : "";
        if (n == 1 && part[0] == '.') { memmove(pending, rest, strlen(rest) + 1); continue; }
        if (n == 2 && !strncmp(part, "..", 2)) {
            while (used > floor && b[used - 1] != '/') --used;
            if (used > floor) --used;
            b[used] = 0;
            memmove(pending, rest, strlen(rest) + 1); continue;
        }
        size_t parent = used;
        if (used + n + 1 >= PATH_MAX) goto toolong;
        b[used++] = '/'; memcpy(b + used, part, n); used += n; b[used] = 0;
        struct stat st;
        /* Raw syscalls avoid recursion through our lstat/readlink wrappers. */
        int found = 0;
        if (!nofollow || *rest) {
            found = !raw_syscall(SYS_newfstatat, AT_FDCWD, b, &st, AT_SYMLINK_NOFOLLOW);
            /* A missing final component is valid for creation; a missing parent is not. */
            if (!found && *rest) return NULL;
        }
        if (found && S_ISLNK(st.st_mode)) {
            if (++links > 40) { errno = ELOOP; return NULL; }
            ssize_t len = raw_syscall(SYS_readlinkat, AT_FDCWD, b, target, sizeof(target) - 1);
            if (len < 0) return NULL;
            target[len] = 0;
            if (snprintf(next, sizeof(next), "%s%s", target, rest) >= PATH_MAX) goto toolong;
            used = parent; b[used] = 0;
            if (target[0] == '/') {
                /* Hooks also create links with physical rootfs targets. */
                if (pre(target, root)) {
                    if (snprintf(next, sizeof(next), "%s%s", target + floor, rest) >= PATH_MAX) goto toolong;
                }
                used = floor; b[used] = 0;
            }
            strcpy(pending, next);
        } else {
            if (found && *rest && !S_ISDIR(st.st_mode)) { errno = ENOTDIR; return NULL; }
            memmove(pending, rest, strlen(rest) + 1);
        }
    }
    return b;
toolong:
    errno = ENAMETOOLONG; return NULL;
}
static char *unmap(char *p) {
    const char *root = getenv("DD_ROOTFS");
    if (p && root && pre(p, root)) {
        size_t n = strlen(root);
        /* Invert only mappings we own; physical HOME (R/root) stays physical. */
        if (system_path(p + n)) memmove(p, p + n, strlen(p + n) + 1);
        else if (!p[n] || root_only(p + n)) strcpy(p, "/");
    }
    return p;
}
/* Renaming or removing "/" would act on the whole rootfs. */
static int is_root(const char *p) { return p && p[0] == '/' && root_only(p) && getenv("DD_ROOTFS"); }
/* A directory fd for "/" is the rootfs (see mapped). Relative paths from it must resolve like
 * absolute ones, or openat(rootfd, "proc/self/status") would land in the rootfs's empty
 * /proc. Every real dirfd is compared with the rootfs (one fstat), so dup, opendir and raw
 * syscalls are covered too. */
static int root_identity(dev_t *dev, ino_t *ino) {
    static dev_t cached_dev; static ino_t cached_ino; static int cached;
    if (__atomic_load_n(&cached, __ATOMIC_ACQUIRE)) { *dev = cached_dev; *ino = cached_ino; return 1; }
    const char *root = getenv("DD_ROOTFS");
    struct stat r;
    if (!root || raw_syscall(SYS_newfstatat, AT_FDCWD, root, &r, 0)) return 0;
    cached_dev = r.st_dev; cached_ino = r.st_ino;
    __atomic_store_n(&cached, 1, __ATOMIC_RELEASE);
    *dev = r.st_dev; *ino = r.st_ino;
    return 1;
}
static const char *dirfd_path(int d, const char *p, char b[PATH_MAX]) {
    if (!p || !*p || p[0] == '/' || d < 0) return p;
    struct stat a;
    dev_t dev; ino_t ino;
    if (!root_identity(&dev, &ino) || fstat(d, &a) || a.st_dev != dev || a.st_ino != ino) return p;
    return snprintf(b, PATH_MAX, "/%s", p) < PATH_MAX ? b : p;
}
#define REAL(name, ret, ...) \
    static ret (*cache_##name)(__VA_ARGS__); \
    ret (*real_##name)(__VA_ARGS__) = __atomic_load_n(&cache_##name, __ATOMIC_ACQUIRE); \
    if (!real_##name) { real_##name = dlsym(RTLD_NEXT, #name); \
        __atomic_store_n(&cache_##name, real_##name, __ATOMIC_RELEASE); } \
    typedef ret result_type __attribute__((unused)); \
    char pathbuf[PATH_MAX] __attribute__((unused)); \
    char dirbuf[PATH_MAX] __attribute__((unused))
#define mapbuf(p, b, f) ({ const char *mp = mapped(p, b, f); \
    if (!mp) { return _Generic((result_type)0, \
        FILE *: NULL, DIR *: NULL, char *: NULL, void *: NULL, default: -1); } mp; })
#define map(p, f) mapbuf(p, pathbuf, f)
#define mapd(d, p, f) mapbuf(dirfd_path(d, p, dirbuf), pathbuf, f)

/* NSS files opens paths internally, bypassing open/fopen interposition. Use the
 * glibc file parsers on streams opened with real fopen, without consulting NSS.
 * Each lookup owns its stream; enumeration and non-reentrant results are per
 * thread. The key destructor closes streams and frees growing result buffers. */
struct nss_state {
    FILE *pw_file, *gr_file;
    struct passwd pw;
    struct group gr;
    char *pw_buf, *gr_buf;
    size_t pw_size, gr_size;
};
static pthread_key_t nss_key;
static pthread_once_t nss_once = PTHREAD_ONCE_INIT;
static int nss_key_error;
static void nss_destroy(void *ptr) {
    struct nss_state *s = ptr;
    if (s->pw_file) fclose(s->pw_file);
    if (s->gr_file) fclose(s->gr_file);
    free(s->pw_buf); free(s->gr_buf); free(s);
}
static void nss_key_init(void) { nss_key_error = pthread_key_create(&nss_key, nss_destroy); }
static struct nss_state *nss_state(void) {
    int rc = pthread_once(&nss_once, nss_key_init);
    if (rc || nss_key_error) { errno = rc ? rc : nss_key_error; return NULL; }
    struct nss_state *s = pthread_getspecific(nss_key);
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) return NULL;
        rc = pthread_setspecific(nss_key, s);
        if (rc) { free(s); errno = rc; return NULL; }
    }
    return s;
}
static FILE *nss_open(int group) {
    REAL(fopen, FILE *, const char *, const char *);
    const char *root = getenv("DD_ROOTFS");
    if (snprintf(pathbuf, sizeof pathbuf, "%s/etc/%s", root, group ? "group" : "passwd") >= (int)sizeof pathbuf) {
        errno = ENAMETOOLONG; return NULL;
    }
    return real_fopen(pathbuf, "re");
}
/* Return ENOENT at end of enumeration, as fget*ent_r does. Rewind a record on
 * ERANGE so callers can grow their buffer and retry without losing an entry. */
static int nss_read(FILE *f, int group, void *entry, char *buf, size_t size, void **result) {
    fpos_t pos;
    *result = NULL;
    if (fgetpos(f, &pos)) return errno;
    int rc;
    if (group) {
        struct group *out = NULL;
        rc = fgetgrent_r(f, entry, buf, size, &out);
        *result = out;
    } else {
        struct passwd *out = NULL;
        rc = fgetpwent_r(f, entry, buf, size, &out);
        /* proot-distro gives the app user /sbin/nologin; natively tmux and others then
         * started panes with it (SHELL=/sbin/nologin, Tab S10 FE). */
        if (!rc && out && out->pw_uid == getuid() && out->pw_shell &&
            (strstr(out->pw_shell, "nologin") || strstr(out->pw_shell, "/false")))
            out->pw_shell = (char *)"/bin/bash";
        *result = out;
    }
    if (rc == ERANGE && fsetpos(f, &pos)) return errno;
    return rc;
}
static int nss_lookup(int group, const char *name, unsigned long id,
                      void *entry, char *buf, size_t size, void **result) {
    *result = NULL;
    FILE *f = nss_open(group);
    if (!f) return errno;
    int rc;
    while (!(rc = nss_read(f, group, entry, buf, size, result))) {
        if (!*result) break;
        const char *found = group ? ((struct group *)entry)->gr_name : ((struct passwd *)entry)->pw_name;
        unsigned long number = group ? ((struct group *)entry)->gr_gid : ((struct passwd *)entry)->pw_uid;
        if (name ? !strcmp(name, found) : id == number) break;
        *result = NULL;
    }
    if (rc) *result = NULL;
    fclose(f);
    return rc == ENOENT ? 0 : rc;
}
#define NSS_LOOKUP(name, type, keytype, group, name_key, id_key) \
int name##_r(keytype key, struct type *entry, char *buf, size_t size, struct type **result) { \
    REAL(name##_r, int, keytype, struct type *, char *, size_t, struct type **); \
    if (!getenv("DD_ROOTFS")) return real_##name##_r(key, entry, buf, size, result); \
    void *out = NULL; \
    int rc = nss_lookup(group, name_key, id_key, entry, buf, size, &out); \
    *result = out; return rc; \
}
NSS_LOOKUP(getpwnam, passwd, const char *, 0, key, 0)
NSS_LOOKUP(getpwuid, passwd, uid_t, 0, NULL, key)
NSS_LOOKUP(getgrnam, group, const char *, 1, key, 0)
NSS_LOOKUP(getgrgid, group, gid_t, 1, NULL, key)

static int nss_grow(char **buf, size_t *size) {
    size_t next = *size ? *size * 2 : 1024;
    if (next < *size) { errno = ENOMEM; return -1; }
    char *p = realloc(*buf, next);
    if (!p) return -1;
    *buf = p; *size = next; return 0;
}
#define NSS_STATIC(name, type, keytype, field) \
struct type *name(keytype key) { \
    REAL(name, struct type *, keytype); \
    if (!getenv("DD_ROOTFS")) return real_##name(key); \
    struct nss_state *s = nss_state(); \
    if (!s) return NULL; \
    struct type *out; int rc; \
    do { \
        if (!s->field##_buf && nss_grow(&s->field##_buf, &s->field##_size)) return NULL; \
        rc = name##_r(key, &s->field, s->field##_buf, s->field##_size, &out); \
        if (rc == ERANGE && nss_grow(&s->field##_buf, &s->field##_size)) return NULL; \
    } while (rc == ERANGE); \
    if (rc) errno = rc; \
    return out; \
}
NSS_STATIC(getpwnam, passwd, const char *, pw)
NSS_STATIC(getpwuid, passwd, uid_t, pw)
NSS_STATIC(getgrnam, group, const char *, gr)
NSS_STATIC(getgrgid, group, gid_t, gr)

#define NSS_ENUM(type, field, group) \
void end##field##ent(void) { \
    REAL(end##field##ent, void, void); \
    if (!getenv("DD_ROOTFS")) { real_end##field##ent(); return; } \
    struct nss_state *s = nss_state(); \
    if (s && s->field##_file) { fclose(s->field##_file); s->field##_file = NULL; } \
} \
void set##field##ent(void) { \
    REAL(set##field##ent, void, void); \
    if (!getenv("DD_ROOTFS")) { real_set##field##ent(); return; } \
    end##field##ent(); \
    struct nss_state *s = nss_state(); \
    if (s) s->field##_file = nss_open(group); \
} \
struct type *get##field##ent(void) { \
    REAL(get##field##ent, struct type *, void); \
    if (!getenv("DD_ROOTFS")) return real_get##field##ent(); \
    struct nss_state *s = nss_state(); \
    if (!s) return NULL; \
    if (!s->field##_file && !(s->field##_file = nss_open(group))) return NULL; \
    void *out; int rc; \
    do { \
        if (!s->field##_buf && nss_grow(&s->field##_buf, &s->field##_size)) return NULL; \
        rc = nss_read(s->field##_file, group, &s->field, s->field##_buf, s->field##_size, &out); \
        if (rc == ERANGE && nss_grow(&s->field##_buf, &s->field##_size)) return NULL; \
    } while (rc == ERANGE); \
    if (rc && rc != ENOENT) errno = rc; \
    return out; \
}
NSS_ENUM(passwd, pw, 0)
NSS_ENUM(group, gr, 1)

int getgrouplist(const char *user, gid_t base, gid_t *groups, int *ngroups) {
    REAL(getgrouplist, int, const char *, gid_t, gid_t *, int *);
    if (!getenv("DD_ROOTFS")) return real_getgrouplist(user, base, groups, ngroups);
    int capacity = *ngroups, count = 1, rc = 0;
    gid_t *ids = malloc(sizeof(*ids));
    if (!ids) return -1;
    ids[0] = base;
    FILE *f = nss_open(1);
    if (!f) { free(ids); return -1; }
    struct group entry;
    char *buf = NULL; size_t size = 0; void *out;
    while (1) {
        if (!buf && nss_grow(&buf, &size)) { rc = errno; break; }
        rc = nss_read(f, 1, &entry, buf, size, &out);
        if (rc == ERANGE) { if (nss_grow(&buf, &size)) { rc = errno; break; } continue; }
        if (rc || !out) break;
        int member = 0, duplicate = 0;
        for (char **m = entry.gr_mem; *m; ++m) if (!strcmp(*m, user)) member = 1;
        for (int i = 0; i < count; ++i) if (ids[i] == entry.gr_gid) duplicate = 1;
        if (!member || duplicate) continue;
        if (count == INT_MAX) { rc = EOVERFLOW; break; }
        gid_t *next = realloc(ids, ((size_t)count + 1) * sizeof(*ids));
        if (!next) { rc = ENOMEM; break; }
        ids = next; ids[count++] = entry.gr_gid;
    }
    fclose(f); free(buf);
    if (rc && rc != ENOENT) { free(ids); errno = rc; return -1; }
    for (int i = 0; i < count && i < capacity; ++i) groups[i] = ids[i];
    free(ids); *ngroups = count;
    return capacity < count ? -1 : count;
}
int initgroups(const char *user, gid_t group) {
    REAL(initgroups, int, const char *, gid_t);
    if (!getenv("DD_ROOTFS")) return real_initgroups(user, group);
    /* Android owns supplementary groups; never call seccomp-blocked setgroups. */
    return 0;
}

static int open_nofollow(int flags) {
    return (flags & O_NOFOLLOW) || (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
}

int open(const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(open, int, const char *, int, ...); return real_open(map(p, open_nofollow(f)), f, m); }
int open64(const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(open64, int, const char *, int, ...); return real_open64(map(p, open_nofollow(f)), f, m); }
int openat(int d, const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(openat, int, int, const char *, int, ...); return real_openat(d, mapd(d, p, open_nofollow(f)), f, m); }
int openat64(int d, const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(openat64, int, int, const char *, int, ...); return real_openat64(d, mapd(d, p, open_nofollow(f)), f, m); }
FILE *fopen(const char *p, const char *m) { REAL(fopen, FILE *, const char *, const char *); return real_fopen(map(p, 0), m); }
FILE *fopen64(const char *p, const char *m) { REAL(fopen64, FILE *, const char *, const char *); return real_fopen64(map(p, 0), m); }
int stat(const char *p, struct stat *s) { REAL(stat, int, const char *, struct stat *); return real_stat(map(p, 0), s); }
int lstat(const char *p, struct stat *s) { REAL(lstat, int, const char *, struct stat *); return real_lstat(map(p, 1), s); }
int stat64(const char *p, struct stat64 *s) { REAL(stat64, int, const char *, struct stat64 *); return real_stat64(map(p, 0), s); }
int lstat64(const char *p, struct stat64 *s) { REAL(lstat64, int, const char *, struct stat64 *); return real_lstat64(map(p, 1), s); }
int fstatat(int d, const char *p, struct stat *s, int f) { REAL(fstatat, int, int, const char *, struct stat *, int); return real_fstatat(d, mapd(d, p, f & AT_SYMLINK_NOFOLLOW), s, f); }
int fstatat64(int d, const char *p, struct stat64 *s, int f) { REAL(fstatat64, int, int, const char *, struct stat64 *, int); return real_fstatat64(d, mapd(d, p, f & AT_SYMLINK_NOFOLLOW), s, f); }
int statx(int d, const char *p, int f, unsigned m, struct statx *s) { REAL(statx, int, int, const char *, int, unsigned, struct statx *); return real_statx(d, mapd(d, p, f & AT_SYMLINK_NOFOLLOW), f, m, s); }
int access(const char *p, int m) { REAL(access, int, const char *, int); return real_access(map(p, 0), m); }
int euidaccess(const char *p, int m) { REAL(euidaccess, int, const char *, int); return real_euidaccess(map(p, 0), m); }
int eaccess(const char *p, int m) { REAL(eaccess, int, const char *, int); return real_eaccess(map(p, 0), m); }
/* Preserve AT_EACCESS and AT_SYMLINK_NOFOLLOW for the real libc call. */
int faccessat(int d, const char *p, int m, int f) { REAL(faccessat, int, int, const char *, int, int); return real_faccessat(d, mapd(d, p, f & AT_SYMLINK_NOFOLLOW), m, f); }
DIR *opendir(const char *p) { REAL(opendir, DIR *, const char *); return real_opendir(map(p, 0)); }
ssize_t readlink(const char *p, char *b, size_t n) { if (*self_exe && is_proc_self_exe(p)) return copy_self_exe(b, n);
    REAL(readlink, ssize_t, const char *, char *, size_t); return real_readlink(map(p, 1), b, n); }
ssize_t readlinkat(int d, const char *p, char *b, size_t n) { if (*self_exe && is_proc_self_exe(p)) return copy_self_exe(b, n);
    REAL(readlinkat, ssize_t, int, const char *, char *, size_t); return real_readlinkat(d, mapd(d, p, 1), b, n); }
char *realpath(const char *p, char *r) { REAL(realpath, char *, const char *, char *); return unmap(real_realpath(map(p, 0), r)); }
int mkdir(const char *p, mode_t m) { REAL(mkdir, int, const char *, mode_t); return real_mkdir(map(p, 1), m); }
int unlink(const char *p) { REAL(unlink, int, const char *); return real_unlink(map(p, 1)); }
int rename(const char *a, const char *b) { REAL(rename, int, const char *, const char *); char second[PATH_MAX];
    if (is_root(a) || is_root(b)) { errno = EBUSY; return -1; }
    const char *ma = map(a, 1); return real_rename(ma, mapbuf(b, second, 1)); }
int __open_2(const char *p, int f) { REAL(__open_2, int, const char *, int); return real___open_2(map(p, open_nofollow(f)), f); }
int __open64_2(const char *p, int f) { REAL(__open64_2, int, const char *, int); return real___open64_2(map(p, open_nofollow(f)), f); }
int __openat_2(int d, const char *p, int f) { REAL(__openat_2, int, int, const char *, int); return real___openat_2(d, mapd(d, p, open_nofollow(f)), f); }
int __openat64_2(int d, const char *p, int f) { REAL(__openat64_2, int, int, const char *, int); return real___openat64_2(d, mapd(d, p, open_nofollow(f)), f); }
int scandir(const char *p, struct dirent ***l, int (*s)(const struct dirent *), int (*c)(const struct dirent **, const struct dirent **)) {
    REAL(scandir, int, const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **)); return real_scandir(map(p, 0), l, s, c); }
int scandir64(const char *p, struct dirent64 ***l, int (*s)(const struct dirent64 *), int (*c)(const struct dirent64 **, const struct dirent64 **)) {
    REAL(scandir64, int, const char *, struct dirent64 ***, int (*)(const struct dirent64 *), int (*)(const struct dirent64 **, const struct dirent64 **)); return real_scandir64(map(p, 0), l, s, c); }
void *dlopen(const char *p, int f) { REAL(dlopen, void *, const char *, int); return real_dlopen(p ? map(p, 0) : NULL, f); }

/* Relative ".." inside the rootfs must not climb above Debian's root into the physical parent
 * of the rootfs; clamp it lexically there, as the resolver does for absolute paths. */
static int clamp_relative(const char *p, char out[PATH_MAX]) {
    const char *root = getenv("DD_ROOTFS");
    if (!root || !p || p[0] == '/') return 0;
    int dots = 0;
    for (const char *s = p; *s;) {
        size_t n = strcspn(s, "/");
        if (n == 2 && s[0] == '.' && s[1] == '.') dots = 1;
        else if (n && !(n == 1 && s[0] == '.')) return 0;
        s += n; while (*s == '/') ++s;
    }
    if (!dots || raw_syscall(SYS_getcwd, out, PATH_MAX) <= 0 || !pre(out, root)) return 0;
    size_t floor = strlen(root), len = strlen(out);
    int escaped = 0;
    for (const char *s = p; *s;) {
        size_t n = strcspn(s, "/");
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            if (len <= floor) escaped = 1;
            while (len > floor && out[len - 1] != '/') --len;
            if (len > floor) --len;
            out[len] = 0;
        } else if (n && !(n == 1 && s[0] == '.')) {
            if (len + n + 2 >= PATH_MAX) return 0;
            out[len++] = '/'; memcpy(out + len, s, n); len += n; out[len] = 0;
        }
        s += n; while (*s == '/') ++s;
    }
    /* Paths that stay inside keep the kernel's semantics (symlinks before ".."). */
    return escaped;
}
int chdir(const char *p) {
    REAL(chdir, int, const char *);
    char clamped[PATH_MAX];
    if (clamp_relative(p, clamped)) return real_chdir(clamped);
    return real_chdir(map(p, 0));
}
char *getcwd(char *b, size_t n) {
    REAL(getcwd, char *, char *, size_t);
    /* Obtain the physical path before applying the caller's logical size limit. */
    char *p = unmap(real_getcwd(NULL, 0));
    if (!p || !b) {
        if (p && n && strlen(p) + 1 > n) { free(p); errno = ERANGE; return NULL; }
        return p;
    }
    if (strlen(p) + 1 > n) { free(p); errno = ERANGE; return NULL; }
    strcpy(b, p); free(p); return b;
}
char *get_current_dir_name(void) { return getcwd(NULL, 0); }
char *canonicalize_file_name(const char *p) { return realpath(p, NULL); }
int mkdirat(int d, const char *p, mode_t m) { REAL(mkdirat, int, int, const char *, mode_t); return real_mkdirat(d, mapd(d, p, 1), m); }
int unlinkat(int d, const char *p, int f) { REAL(unlinkat, int, int, const char *, int); const char *pp = dirfd_path(d, p, dirbuf);
    if (is_root(pp)) { errno = EBUSY; return -1; } return real_unlinkat(d, map(pp, 1), f); }
int rmdir(const char *p) { REAL(rmdir, int, const char *); if (is_root(p)) { errno = EBUSY; return -1; } return real_rmdir(map(p, 1)); }
int chmod(const char *p, mode_t m) { REAL(chmod, int, const char *, mode_t); return real_chmod(map(p, 0), m); }
int fchmodat(int d, const char *p, mode_t m, int f) { REAL(fchmodat, int, int, const char *, mode_t, int); return real_fchmodat(d, mapd(d, p, f & AT_SYMLINK_NOFOLLOW), m, f); }
int chown(const char *p, uid_t u, gid_t g) { REAL(chown, int, const char *, uid_t, gid_t); return real_chown(map(p, 0), u, g); }
int lchown(const char *p, uid_t u, gid_t g) { REAL(lchown, int, const char *, uid_t, gid_t); return real_lchown(map(p, 1), u, g); }
int fchownat(int d, const char *p, uid_t u, gid_t g, int f) { REAL(fchownat, int, int, const char *, uid_t, gid_t, int); return real_fchownat(d, mapd(d, p, f & AT_SYMLINK_NOFOLLOW), u, g, f); }
int utimensat(int d, const char *p, const struct timespec t[2], int f) { REAL(utimensat, int, int, const char *, const struct timespec *, int); return real_utimensat(d, mapd(d, p, f & AT_SYMLINK_NOFOLLOW), t, f); }
int utimes(const char *p, const struct timeval t[2]) { REAL(utimes, int, const char *, const struct timeval *); return real_utimes(map(p, 0), t); }
int truncate(const char *p, off_t n) { REAL(truncate, int, const char *, off_t); return real_truncate(map(p, 0), n); }
int truncate64(const char *p, off64_t n) { REAL(truncate64, int, const char *, off64_t); return real_truncate64(map(p, 0), n); }
int statfs(const char *p, struct statfs *s) { REAL(statfs, int, const char *, struct statfs *); return real_statfs(map(p, 0), s); }
int statfs64(const char *p, struct statfs64 *s) { REAL(statfs64, int, const char *, struct statfs64 *); return real_statfs64(map(p, 0), s); }
int symlink(const char *a, const char *b) {
    REAL(symlink, int, const char *, const char *); char second[PATH_MAX];
    return real_symlink(map(a, 1), mapbuf(b, second, 1));
}
int symlinkat(const char *a, int d, const char *b) {
    REAL(symlinkat, int, const char *, int, const char *); char second[PATH_MAX];
    return real_symlinkat(map(a, 1), d, mapbuf(dirfd_path(d, b, dirbuf), second, 1));
}
/* Android forbids hard links for apps (EPERM). A symlink, like proot without its link2symlink
 * bookkeeping, broke "link, then unlink the source" (npm's cacache, git objects): the
 * remaining name dangled (Tab S10 FE, fold.101). Publish a copy instead, atomically and only
 * when the name is free, as link() would. WebKit's cache in OrcaSlicer needs a fallback too.
 * Paths arrive mapped, so raw system calls keep them from being mapped twice. */
static int publish_copy(int olddir, const char *old, int newdir, const char *new, int follow, int link_errno) {
    static unsigned counter;
    struct stat existing, source;
    /* An existing name fails before any copying, as link() does (git and npm expect EEXIST). */
    if (!fstatat(newdir, new, &existing, AT_SYMLINK_NOFOLLOW)) { errno = EEXIST; return -1; }
    if (errno != ENOENT) return -1;
    if (fstatat(olddir, old, &source, follow ? 0 : AT_SYMLINK_NOFOLLOW)) return -1;
    /* A short temporary name in the destination directory: suffixing a long name could
     * exceed NAME_MAX. */
    char tmp[PATH_MAX];
    const char *slash = strrchr(new, '/');
    int dirlen = slash ? (int)(slash - new + 1) : 0;
    if (snprintf(tmp, sizeof tmp, "%.*s.ddlink-%x-%x", dirlen, new, (unsigned)getpid(),
                 __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED)) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG; return -1;
    }
    int src = (int)raw_syscall(SYS_openat, olddir, old, O_RDONLY | O_CLOEXEC | O_NONBLOCK | (follow ? 0 : O_NOFOLLOW));
    if (src < 0) {
        if (errno != ELOOP || follow) return -1;
        /* link() without AT_SYMLINK_FOLLOW links the symlink itself. */
        char target[PATH_MAX];
        ssize_t n = (ssize_t)raw_syscall(SYS_readlinkat, olddir, old, target, sizeof target - 1);
        if (n < 0) return -1;
        target[n] = 0;
        if (raw_syscall(SYS_symlinkat, target, newdir, tmp) < 0) return -1;
    } else {
        struct stat st;
        int e = fstat(src, &st) ? errno : S_ISREG(st.st_mode) ? 0 : EPERM;
        if (e) { close(src); errno = e; return -1; }
        int dst = (int)raw_syscall(SYS_openat, newdir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (dst < 0) { e = errno; close(src); errno = e; return -1; }
        char buf[65536];
        ssize_t n = 0;
        int ok = 1;
        while (ok && (n = read(src, buf, sizeof buf)) != 0) {
            if (n < 0) { if (errno == EINTR) continue; ok = 0; break; }
            for (ssize_t done = 0; done < n;) {
                ssize_t w = write(dst, buf + done, (size_t)(n - done));
                if (w < 0) { if (errno == EINTR) continue; ok = 0; break; }
                done += w;
            }
        }
        struct timespec times[2] = {st.st_atim, st.st_mtim};
        if (ok && (fchmod(dst, st.st_mode & 07777) || futimens(dst, times))) ok = 0;
        e = errno;
        close(src);
        if (close(dst)) { if (ok) e = errno; ok = 0; }
        if (!ok) { raw_syscall(SYS_unlinkat, newdir, tmp, 0); errno = e; return -1; }
    }
    /* A hard link never crosses file systems; neither does its copy. */
    struct stat copy;
    long rc = fstatat(newdir, tmp, &copy, AT_SYMLINK_NOFOLLOW);
    if (!rc && copy.st_dev != source.st_dev) { errno = EXDEV; rc = -1; }
    /* Without RENAME_NOREPLACE a plain rename could replace a concurrent publisher's file:
     * report the original link error instead. */
    if (!rc) rc = raw_syscall(SYS_renameat2, newdir, tmp, newdir, new, 1 /* RENAME_NOREPLACE */);
    if (rc < 0 && (errno == EINVAL || errno == ENOSYS)) errno = link_errno;
    if (rc < 0) { int e = errno; raw_syscall(SYS_unlinkat, newdir, tmp, 0); errno = e; return -1; }
    return 0;
}
int link(const char *a, const char *b) {
    REAL(link, int, const char *, const char *); char second[PATH_MAX];
    const char *ma = map(a, 1), *mb = mapbuf(b, second, 1);
    int rc = real_link(ma, mb);
    if (rc < 0 && (errno == EPERM || errno == EACCES)) rc = publish_copy(AT_FDCWD, ma, AT_FDCWD, mb, 0, errno);
    return rc;
}
int linkat(int a, const char *p, int b, const char *q, int f) {
    REAL(linkat, int, int, const char *, int, const char *, int); char second[PATH_MAX], dirbuf2[PATH_MAX];
    const char *mp = mapd(a, p, !(f & AT_SYMLINK_FOLLOW)), *mq = mapbuf(dirfd_path(b, q, dirbuf2), second, 1);
    int rc = real_linkat(a, mp, b, mq, f);
    if (rc < 0 && (errno == EPERM || errno == EACCES) && !(f & AT_EMPTY_PATH))
        rc = publish_copy(a, mp, b, mq, f & AT_SYMLINK_FOLLOW, errno);
    return rc;
}

/* nftw/ftw walk the tree with libc-internal calls past the hook: map the start path (zutty
 * walks /usr/share/fonts). */
int nftw(const char *p, int (*fn)(const char *, const struct stat *, int, struct FTW *), int fds, int flags) {
    REAL(nftw, int, const char *, int (*)(const char *, const struct stat *, int, struct FTW *), int, int);
    return real_nftw(map(p, 0), fn, fds, flags);
}
int nftw64(const char *p, int (*fn)(const char *, const struct stat64 *, int, struct FTW *), int fds, int flags) {
    REAL(nftw64, int, const char *, int (*)(const char *, const struct stat64 *, int, struct FTW *), int, int);
    return real_nftw64(map(p, 0), fn, fds, flags);
}
int ftw(const char *p, int (*fn)(const char *, const struct stat *, int), int fds) {
    REAL(ftw, int, const char *, int (*)(const char *, const struct stat *, int), int);
    return real_ftw(map(p, 0), fn, fds);
}

/* Calls through the generic syscall() (Mesa: kcmp, Qt/OpenCASCADE: mbind) are not inline svc
 * sites, so patch-glibc cannot reach them: SolidCraft and FuCad died with SIGSYS. Return
 * ENOSYS for Android's seccomp blocklist (accept stays: it is mapped to accept4 above). */
static int seccomp_blocked(long nr) {
#if defined(__aarch64__)
    static const short blocked[] = {
        18, 39, 40, 42, 51, 58, 89, 99, 100, 104, 105, 106, 112, 116, 142, 143, 144, 145, 146,
        149, 151, 152, 159, 161, 162, 170, 171, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189,
        190, 191, 192, 193, 194, 195, 196, 197, 217, 218, 219, 224, 225, 234, 235, 236, 237,
        238, 239, 244, 262, 263, 264, 265, 266, 272, 273, 288, 289, 290, 292, 293, 294, 425,
        426, 427, 428, 429, 430, 431, 432, 433, 442, 443, 447, 449, 450, 451, 453, 454, 455,
        456, 457, 458, 459, 460,
    };
#else /* the tests run on x86_64: the two calls seen on the device */
    static const short blocked[] = { __NR_kcmp, __NR_mbind };
#endif
    for (size_t i = 0; i < sizeof blocked / sizeof blocked[0]; ++i)
        if (blocked[i] == nr) return 1;
    return 0;
}
long syscall(long nr, ...) {
    va_list ap;
    va_start(ap, nr);
    long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
    long d = va_arg(ap, long), e = va_arg(ap, long), f = va_arg(ap, long);
    va_end(ap);
    if (seccomp_blocked(nr)) { errno = ENOSYS; return -1; }
    /* libuv calls statx through syscall(): Node's fs.stat/lstat('/usr') failed with ENOENT
     * and npm's arborist with it (Tab S10 FE). Map the path of the path-taking calls. */
    char path[PATH_MAX], relative[PATH_MAX];
    const char *p = (const char *)b, *mp = NULL;
    /* Only the path-taking calls below have a dirfd in a and a path in b. */
#define AT(p) dirfd_path((int)a, p, relative)
    switch (nr) {
    case SYS_statx: mp = mapped(AT(p), path, c & AT_SYMLINK_NOFOLLOW); break;
#ifdef SYS_newfstatat
    case SYS_newfstatat: mp = mapped(AT(p), path, d & AT_SYMLINK_NOFOLLOW); break;
#endif
    case SYS_faccessat: mp = mapped(AT(p), path, 0); break;
#ifdef SYS_faccessat2
    case SYS_faccessat2: mp = mapped(AT(p), path, d & AT_SYMLINK_NOFOLLOW); break;
#endif
    case SYS_readlinkat: mp = mapped(AT(p), path, 1); break;
    case SYS_openat: mp = mapped(AT(p), path, open_nofollow((int)c)); break;
    default: p = NULL; break;
    }
#undef AT
    if (p) { if (!mp) return -1; b = (long)mp; }
    REAL(syscall, long, long, ...);
    return real_syscall(nr, a, b, c, d, e, f);
}
int renameat(int a, const char *p, int b, const char *q) {
    REAL(renameat, int, int, const char *, int, const char *); char second[PATH_MAX], dirbuf2[PATH_MAX];
    const char *pp = dirfd_path(a, p, dirbuf), *qq = dirfd_path(b, q, dirbuf2);
    if (is_root(pp) || is_root(qq)) { errno = EBUSY; return -1; }
    return real_renameat(a, map(pp, 1), b, mapbuf(qq, second, 1));
}
int renameat2(int a, const char *p, int b, const char *q, unsigned f) {
    REAL(renameat2, int, int, const char *, int, const char *, unsigned); char second[PATH_MAX], dirbuf2[PATH_MAX];
    const char *pp = dirfd_path(a, p, dirbuf), *qq = dirfd_path(b, q, dirbuf2);
    if (is_root(pp) || is_root(qq)) { errno = EBUSY; return -1; }
    return real_renameat2(a, map(pp, 1), b, mapbuf(qq, second, 1), f);
}

/* Socket names are length-delimited, unlike file paths. Keep abstract, unnamed,
 * malformed and overlong addresses unchanged, including their original length. */
static const struct sockaddr *socket_address(const struct sockaddr *addr, socklen_t *len,
                                            struct sockaddr_un *out) {
    size_t offset = offsetof(struct sockaddr_un, sun_path);
    if (!addr || *len <= offset || *len > sizeof(*out) || addr->sa_family != AF_UNIX) return addr;
    const struct sockaddr_un *un = (const struct sockaddr_un *)addr;
    size_t n = *len - offset;
    if (!un->sun_path[0]) return addr;
    char path[sizeof(un->sun_path) + 1], buffer[PATH_MAX];
    memcpy(path, un->sun_path, n); path[n] = 0;
    int saved_errno = errno;
    const char *p = mapped(path, buffer, 0);
    errno = saved_errno;
    if (!p || !strcmp(p, path) || strlen(p) >= sizeof(out->sun_path)) return addr;
    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    strcpy(out->sun_path, p);
    *len = (socklen_t)(offset + strlen(p) + 1);
    return (const struct sockaddr *)out;
}
int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    REAL(connect, int, int, const struct sockaddr *, socklen_t);
    struct sockaddr_un out;
    addr = socket_address(addr, &len, &out);
    return real_connect(fd, addr, len);
}
int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    REAL(bind, int, int, const struct sockaddr *, socklen_t);
    struct sockaddr_un out;
    addr = socket_address(addr, &len, &out);
    return real_bind(fd, addr, len);
}
/* Android's seccomp kills accept (202) but allows accept4: the tmux server died as soon as a
 * client connected (Tab S10 FE). accept() is accept4() without flags. */
int accept(int fd, struct sockaddr *addr, socklen_t *len) { return accept4(fd, addr, len, 0); }
ssize_t sendto(int fd, const void *buf, size_t size, int flags, const struct sockaddr *addr, socklen_t len) {
    REAL(sendto, ssize_t, int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    struct sockaddr_un out;
    addr = socket_address(addr, &len, &out);
    return real_sendto(fd, buf, size, flags, addr, len);
}
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    REAL(sendmsg, ssize_t, int, const struct msghdr *, int);
    if (!msg) return real_sendmsg(fd, msg, flags);
    struct msghdr copy = *msg;
    struct sockaddr_un out;
    copy.msg_name = (void *)socket_address(msg->msg_name, &copy.msg_namelen, &out);
    return real_sendmsg(fd, &copy, flags);
}

/* Prepare before exec/spawn, so spawn file actions and attributes retain libc
 * semantics. No fork callbacks or process-wide environment changes are used. */
struct command { char *file; char **argv; char **env; };
static void free_vector(char **v) {
    if (v) { for (size_t i = 0; v[i]; ++i) free(v[i]); free(v); }
}
static void dispose(struct command *c) { free(c->file); free_vector(c->argv); free_vector(c->env); }
static size_t count(char *const v[]) { size_t n = 0; if (v) while (v[n]) ++n; return n; }
static char **copy_vector(const char *const v[], size_t n) {
    char **r = calloc(n + 1, sizeof(*r));
    if (!r) return NULL;
    for (size_t i = 0; i < n; ++i) if (!(r[i] = strdup(v[i]))) { free_vector(r); return NULL; }
    return r;
}
static int envkey(const char *entry, const char *key) {
    size_t n = strlen(key); return !strncmp(entry, key, n) && entry[n] == '=';
}
static char **child_env(char *const env[], int bionic) {
    const char *runtime[] = {"DD_NATIVE_ACTIVE", "DD_ROOTFS", "DD_TMP", "DD_PLIB", "DD_LIBPATH",
        "DD_SAVED_LD_PRELOAD", "DD_SAVED_LD_LIBRARY_PATH", "DD_SAVED_PATH", NULL};
    size_t n = count(env), k = 0;
    size_t saved = 0;
    for (char **e = environ; *e; ++e) if (!strncmp(*e, "DD_SAVED_", 9)) ++saved;
    char **v = calloc(n + 13 + 2 * saved, sizeof(*v));
    if (!v) return NULL;
    for (size_t i = 0; i < n; ++i) {
        int config = 0;
        for (int j = 0; runtime[j]; ++j) if (envkey(env[i], runtime[j])) config = 1;
        if (config || !strncmp(env[i], "DD_SAVED_", 9)) continue;
        if (bionic && (envkey(env[i], "PREFIX") || !strncmp(env[i], "TERMUX_", 7))) continue;
        if (envkey(env[i], "LD_PRELOAD") || envkey(env[i], "LD_LIBRARY_PATH") ||
            (bionic && envkey(env[i], "PATH"))) continue;
        if (bionic && (envkey(env[i], "LOCPATH") || envkey(env[i], "TZDIR") ||
            envkey(env[i], "FONTCONFIG_SYSROOT") || envkey(env[i], "FONTCONFIG_FILE") ||
            envkey(env[i], "SSL_CERT_FILE") || envkey(env[i], "GCONV_PATH") ||
            envkey(env[i], "GLIBC_TUNABLES") || envkey(env[i], "PYTHONHOME") ||
            envkey(env[i], "PERL5LIB"))) continue;
        if (!(v[k++] = strdup(env[i]))) { free_vector(v); return NULL; }
    }
    /* subprocess(env=...) still needs the native runtime in its descendants. */
    for (int i = 0; runtime[i]; ++i) {
        const char *s = getenv(runtime[i]);
        if (s) {
            if (asprintf(&v[k], "%s=%s", runtime[i], s) < 0) { v[k] = NULL; free_vector(v); return NULL; }
            ++k;
        }
    }
    for (char **e = environ; *e; ++e) {
        if (strncmp(*e, "DD_SAVED_", 9)) continue;
        if (envkey(*e, "DD_SAVED_PATH") || envkey(*e, "DD_SAVED_LD_PRELOAD") ||
            envkey(*e, "DD_SAVED_LD_LIBRARY_PATH")) continue;
        if (!(v[k++] = strdup(*e))) { free_vector(v); return NULL; }
        if (bionic && !(v[k++] = strdup(*e + 9))) { free_vector(v); return NULL; }
    }
    if (bionic) {
        const char *keys[] = {"LD_PRELOAD", "LD_LIBRARY_PATH", "PATH"};
        const char *saved[] = {"DD_SAVED_LD_PRELOAD", "DD_SAVED_LD_LIBRARY_PATH", "DD_SAVED_PATH"};
        for (int i = 0; i < 3; ++i) {
            const char *s = getenv(saved[i]);
            if (s && *s && asprintf(&v[k], "%s=%s", keys[i], s) < 0) { v[k] = NULL; free_vector(v); return NULL; }
            if (s && *s) ++k;
        }
    }
    return v;
}
/* Inspection bypasses the wrappers: the target has already been mapped. */
static int inspect(const char *file, char interp[PATH_MAX], char script[256]) {
    int (*raw_open)(const char *, int, ...) = dlsym(RTLD_NEXT, "open");
    int fd = raw_open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st)) { int e = errno; close(fd); errno = e; return -1; }
    if (!S_ISREG(st.st_mode)) { close(fd); errno = EACCES; return -1; }
    Elf64_Ehdr eh;
    ssize_t n = pread(fd, &eh, sizeof(eh), 0);
    int result = 0;
    interp[0] = script[0] = 0;
    if (n >= 2 && ((char *)&eh)[0] == '#' && ((char *)&eh)[1] == '!') {
        n = pread(fd, script, 255, 0);
        if (n < 0) result = -1;
        else { script[n] = 0; result = 2; }
    } else if (n == (ssize_t)sizeof(eh) && !memcmp(eh.e_ident, ELFMAG, SELFMAG) &&
               eh.e_ident[EI_CLASS] == ELFCLASS64 && eh.e_ident[EI_DATA] == ELFDATA2LSB &&
               eh.e_phentsize == sizeof(Elf64_Phdr)) {
        result = 1;
        for (unsigned i = 0; i < eh.e_phnum; ++i) {
            Elf64_Phdr ph;
            if (pread(fd, &ph, sizeof(ph), eh.e_phoff + (uint64_t)i * sizeof(ph)) != (ssize_t)sizeof(ph)) { errno = ENOEXEC; result = -1; break; }
            if (ph.p_type != PT_INTERP) continue;
            if (!ph.p_filesz || ph.p_filesz > PATH_MAX ||
                pread(fd, interp, ph.p_filesz, ph.p_offset) != (ssize_t)ph.p_filesz || interp[ph.p_filesz - 1]) {
                errno = ENOEXEC; result = -1;
            }
            break;
        }
    }
    int e = errno; close(fd); errno = e; return result;
}
/* Programs without glibc's loader (static Go, musl, static-pie) issue system calls this hook
 * never sees: Debian paths would reach Android unmapped. Run them under proot with the same
 * rootfs instead; their environment is passed through env -i, so proot's own Termux
 * environment stays out of the program. Physical rootfs and /tmp paths in arguments and
 * environment values are invisible inside proot and become their Debian paths. */
static int virtual_path(const char *physical, const char *root, const char *tmp, char out[PATH_MAX]) {
    if (pre(physical, root)) {
        const char *rest = physical + strlen(root);
        return snprintf(out, PATH_MAX, "%s", *rest ? rest : "/") < PATH_MAX;
    }
    if (tmp && pre(physical, tmp))
        return snprintf(out, PATH_MAX, "/tmp%s", physical + strlen(tmp)) < PATH_MAX;
    return 0;
}
/* NAME=VALUE or a plain argument; colon lists (PATH) are translated per component. */
static char *guest_string(const char *s, const char *root, const char *tmp, int is_env) {
    const char *value = s;
    if (is_env) { const char *eq = strchr(s, '='); value = eq ? eq + 1 : s + strlen(s); }
    size_t cap = strlen(s) + 1;
    char *out = malloc(cap), part[PATH_MAX], mapped_part[PATH_MAX];
    if (!out) return NULL;
    size_t len = (size_t)(value - s);
    memcpy(out, s, len);
    for (const char *p = value;;) {
        const char *end = is_env ? strchr(p, ':') : NULL;
        size_t n = end ? (size_t)(end - p) : strlen(p);
        const char *piece = p;
        if (n < sizeof part) {
            memcpy(part, p, n); part[n] = 0;
            if (virtual_path(part, root, tmp, mapped_part)) { piece = mapped_part; n = strlen(mapped_part); }
        }
        if (len + n + 2 > cap) {
            cap = (len + n + 2) * 2;
            char *grown = realloc(out, cap);
            if (!grown) { free(out); return NULL; }
            out = grown;
        }
        memcpy(out + len, piece, n); len += n;
        if (!end) break;
        out[len++] = ':';
        p = end + 1;
    }
    out[len] = 0;
    return out;
}
/* Package management and root tools need proot's fake root: scripts started natively (the
 * Claude Code installer, craft-install.sh, sudo apt in a native terminal) ran apt and dpkg
 * without it. The root and package part of proot_only in droiddesk-glibc-native; Wine and
 * the x86 emulators keep their launcher route (proot-distro with its full bind list). */
static int proot_only_name(const char *file) {
    static const char *names[] = {"apt", "apt-get", "apt-cache", "aptitude", "dpkg", "dpkg-reconfigure",
        "sudo", "su", "passwd", "useradd", "usermod", "groupadd", "chroot", "update-alternatives", NULL};
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;
    for (int i = 0; names[i]; ++i) if (!strcmp(base, names[i])) return 1;
    return 0;
}
static int prepare_proot(const char *target, char *const argv[], char *const env[], struct command *c, int fake_root) {
    const char *root = getenv("DD_ROOTFS"), *tmp = getenv("DD_TMP"), *prefix = getenv("DD_SAVED_PREFIX");
    char proot[PATH_MAX], loader[PATH_MAX], program[PATH_MAX], cwd[PATH_MAX], vcwd[PATH_MAX], absolute[PATH_MAX];
    if (!root || !tmp || !prefix) { errno = ENOENT; return -1; }
    long len = raw_syscall(SYS_getcwd, cwd, sizeof cwd);
    if (len <= 0) strcpy(cwd, "/");
    if (target[0] != '/') {
        /* ./tool: mapped() leaves relative names alone. */
        if (snprintf(absolute, sizeof absolute, "%s/%s", cwd, target) >= (int)sizeof absolute) { errno = ENAMETOOLONG; return -1; }
        target = absolute;
    }
    /* Outside the rootfs (npm packages in the Termux home: esbuild, turbo, biome) the program
     * keeps its path and its directory is bound in. Termux's own static files stay native. */
    char program_dir[PATH_MAX] = "";
    if (!virtual_path(target, root, tmp, program)) {
        const char *slash = strrchr(target, '/');
        if (pre(target, prefix) || !slash || strlen(target) >= sizeof program) { errno = ENOENT; return -1; }
        strcpy(program, target);
        snprintf(program_dir, sizeof program_dir, "%.*s", (int)(slash - target), target);
        if (!*program_dir) strcpy(program_dir, "/");
    }
    if (snprintf(proot, sizeof proot, "%s/bin/proot", prefix) >= (int)sizeof proot ||
        snprintf(loader, sizeof loader, "%s/libexec/proot/loader", prefix) >= (int)sizeof loader ||
        access(proot, X_OK)) { errno = ENOENT; return -1; }
    int outside = !virtual_path(cwd, root, tmp, vcwd);
    if (outside) strcpy(vcwd, cwd);
    char **program_env = child_env(env, 0);
    if (!program_env) return -1;
    size_t n = count(argv), envs = count(program_env);
    const char *argv0 = n ? argv[0] : program, *base = strrchr(program, '/');
    const char *a0base = strrchr(argv0, '/');
    /* Multicall programs (BusyBox) select by argv[0]; env cannot set it, bash's exec -a can. */
    int keep_argv0 = strcmp(a0base ? a0base + 1 : argv0, base ? base + 1 : program) != 0;
    char **v = calloc(n + envs + 40, sizeof *v);
    if (!v) { free_vector(program_env); errno = ENOMEM; return -1; }
    size_t k = 0;
    int ok = 1;
#define ADD(s) do { if (ok && !(v[k++] = strdup(s))) ok = 0; } while (0)
    ADD(proot); ADD("-L"); ADD("--link2symlink"); ADD("--sysvipc"); ADD("--kill-on-exit");
    if (fake_root) ADD("-0");
    ADD("-r"); ADD(root);
    ADD("-b"); ADD("/dev"); ADD("-b"); ADD("/proc"); ADD("-b"); ADD("/sys");
    ADD("-b"); if (ok && asprintf(&v[k++], "%s:/tmp", tmp) < 0) { v[k - 1] = NULL; ok = 0; }
    ADD("-b"); ADD("/storage");
    if (outside) { ADD("-b"); ADD(cwd); }
    if (*program_dir && (!outside || strcmp(program_dir, cwd))) { ADD("-b"); ADD(program_dir); }
    ADD("-w"); ADD(vcwd);
    ADD("/usr/bin/env"); ADD("-i");
    for (size_t i = 0; ok && i < envs; ++i) {
        if (!strncmp(program_env[i], "DD_", 3)) continue;
        if (!(v[k++] = guest_string(program_env[i], root, tmp, 1))) ok = 0;
    }
    if (keep_argv0) { ADD("/bin/bash"); ADD("-c"); ADD("exec -a \"$0\" \"$@\""); ADD(argv0); }
    ADD(program);
    for (size_t i = 1; ok && i < n; ++i)
        if (!(v[k++] = guest_string(argv[i], root, tmp, 0))) ok = 0;
#undef ADD
    free_vector(program_env);
    if (!ok) { free_vector(v); errno = ENOMEM; return -1; }
    c->argv = v;
    /* proot itself is a Termux program: Termux environment plus its loader and scratch dir. */
    char **bionic_env = child_env(env, 1);
    /* proot passes its environment to the guest's /usr/bin/env, whose glibc loader printed an
     * error for the Termux socket hook in LD_PRELOAD (Tab S10 FE). proot needs neither:
     * its RUNPATH points at the Termux libraries. */
    size_t m = 0;
    for (size_t i = 0; bionic_env && bionic_env[i]; ++i) {
        if (envkey(bionic_env[i], "LD_PRELOAD") || envkey(bionic_env[i], "LD_LIBRARY_PATH")) free(bionic_env[i]);
        else bionic_env[m++] = bionic_env[i];
    }
    if (bionic_env) bionic_env[m] = NULL;
    char **grown = bionic_env ? realloc(bionic_env, (m + 3) * sizeof *bionic_env) : NULL;
    if (!grown) { free_vector(bionic_env); errno = ENOMEM; return -1; }
    c->env = grown;
    c->env[m] = c->env[m + 1] = c->env[m + 2] = NULL;
    if (asprintf(&c->env[m], "PROOT_LOADER=%s", loader) < 0) { c->env[m] = NULL; errno = ENOMEM; return -1; }
    if (asprintf(&c->env[m + 1], "PROOT_TMP_DIR=%s/proot", tmp) < 0) { c->env[m + 1] = NULL; errno = ENOMEM; return -1; }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/proot", tmp);
    raw_syscall(SYS_mkdirat, AT_FDCWD, dir, 0700);
    c->file = strdup(proot);
    if (!c->file) { errno = ENOMEM; return -1; }
    return 0;
}
static int prepare(const char *file, char *const argv[], char *const env[], struct command *c, int depth) {
    if (depth > 8) { errno = ELOOP; return -1; }
    if (*self_exe && (is_proc_self_exe(file) ||
                      (is_loader(file) && !(count(argv) > 1 && ld_option(argv[1])))))
        file = self_exe;
    char path[PATH_MAX], interp[PATH_MAX], script[256];
    const char *target = mapped(file, path, 0);
    if (!target) return -1;
    if (access(target, X_OK)) return -1;
    if (depth == 0 && proot_only_name(file)) {
        if (!prepare_proot(target, argv, env, c, 1)) return 0;
        if (errno != ENOENT) return -1;
        dispose(c); memset(c, 0, sizeof *c);
    }
    int kind = inspect(target, interp, script);
    /* Samsung lets apps execute but not read /system/bin: reading the ELF header failed with
     * EPERM, so sh and toybox could not start (Tab S10 FE). Android partitions hold only
     * bionic programs. */
    if (kind < 0 && (errno == EPERM || errno == EACCES) &&
        (pre(target, "/system") || pre(target, "/apex") || pre(target, "/vendor") ||
         pre(target, "/product") || pre(target, "/odm") || pre(target, "/system_ext"))) {
        kind = 1;
        strcpy(interp, "/system/bin/linker64");
    }
    if (kind < 0) return -1;
    size_t n = count(argv);
    if (kind == 2) {
        char *s = script + 2;
        while (*s == ' ' || *s == '\t') ++s;
        char *end = strpbrk(s, "\r\n"); if (end) *end = 0;
        end = s + strlen(s);
        while (end > s && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        char *arg = s; while (*arg && *arg != ' ' && *arg != '\t') ++arg;
        if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') ++arg; }
        if (!*s) { errno = ENOEXEC; return -1; }
        const char **v = calloc(n + 4, sizeof(*v)); if (!v) return -1;
        size_t k = 0; v[k++] = s; if (*arg) v[k++] = arg;
        v[k++] = target;
        for (size_t i = 1; i < n; ++i) v[k++] = argv[i];
        int rc = prepare(s, (char *const *)v, env, c, depth + 1);
        int e = errno; free(v); errno = e; return rc;
    }
    int glibc = !strcmp(interp, "/lib/ld-linux-aarch64.so.1") || !strcmp(interp, "/lib64/ld-linux-x86-64.so.2");
    int bionic = !strcmp(interp, "/system/bin/linker64") || !strcmp(interp, "/system/bin/linker");
    if (kind == 1 && !glibc && !bionic && !is_loader(target)) {
        if (!prepare_proot(target, argv, env, c, 0)) return 0;
        if (errno != ENOENT) return -1;
        dispose(c); memset(c, 0, sizeof *c);
    }
    c->env = child_env(env, bionic);
    if (!c->env) return -1;
    if (glibc) {
        const char *plib = getenv("DD_PLIB"), *lp = getenv("DD_LIBPATH");
        Dl_info own;
        if (!plib || !lp || !dladdr((void *)prepare, &own) || !own.dli_fname) { errno = EINVAL; return -1; }
        if (asprintf(&c->file, "%s/ld.so", plib) < 0) { c->file = NULL; return -1; }
        /* dladdr may return a relative name. Resolve it before a child changes cwd. */
        char *(*raw_realpath)(const char *, char *) = dlsym(RTLD_NEXT, "realpath");
        char *hook = raw_realpath(own.dli_fname, NULL);
        if (!hook) return -1;
        const char **v = calloc(n + 10, sizeof(*v)); if (!v) { free(hook); return -1; }
        size_t k = 0;
        v[k++] = c->file; v[k++] = "--library-path"; v[k++] = lp;
        v[k++] = "--preload"; v[k++] = hook;
        v[k++] = "--argv0"; v[k++] = n ? argv[0] : target; v[k++] = target;
        for (size_t i = 1; i < n; ++i) v[k++] = argv[i];
        c->argv = copy_vector(v, k); free(v); free(hook);
    } else {
        /* Patched copies carry this hook in DT_NEEDED. Android binaries restore
         * the saved Termux loader variables and PATH, including custom envp. */
        c->file = strdup(target);
        c->argv = copy_vector((const char *const *)argv, n);
    }
    return c->file && c->argv ? 0 : -1;
}
static int select_command(const char *file, char *const argv[], char *const env[], int search, struct command *c) {
    if (!search || strchr(file, '/')) return prepare(file, argv, env, c, 0);
    if (!*file) { errno = ENOENT; return -1; }
    const char *path = getenv("PATH"); if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    int denied = 0;
    do {
        const char *end = strchr(path, ':'); size_t len = end ? (size_t)(end - path) : strlen(path);
        char candidate[PATH_MAX];
        int n = len ? snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)len, path, file) : snprintf(candidate, sizeof(candidate), "%s", file);
        if (n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
        /* access prevents choosing a non-executable first PATH match. */
        if (!access(candidate, X_OK)) {
            if (!prepare(candidate, argv, env, c, 0)) return 0;
            int e = errno; dispose(c); memset(c, 0, sizeof(*c)); errno = e;
            if (e != ENOENT && e != ENOTDIR && e != EACCES) return -1;
        }
        if (errno == EACCES) denied = 1;
        else if (errno != ENOENT && errno != ENOTDIR) return -1;
        if (!end) break;
        path = end + 1;
    } while (1);
    errno = denied ? EACCES : ENOENT; return -1;
}
static int execute(const char *file, char *const argv[], char *const env[], int search) {
    struct command c = {0};
    int rc = select_command(file, argv, env, search, &c);
    if (!rc) {
        int (*raw_execve)(const char *, char *const [], char *const []) = dlsym(RTLD_NEXT, "execve");
        rc = raw_execve(c.file, c.argv, c.env);
    }
    int e = errno; dispose(&c); errno = e; return rc;
}
int execve(const char *p, char *const a[], char *const e[]) { return execute(p, a, e, 0); }
int execv(const char *p, char *const a[]) { return execute(p, a, environ, 0); }
int execvp(const char *p, char *const a[]) { return execute(p, a, environ, 1); }
int execvpe(const char *p, char *const a[], char *const e[]) { return execute(p, a, e, 1); }
static int spawn_command(pid_t *pid, const char *file, const posix_spawn_file_actions_t *actions,
                         const posix_spawnattr_t *attrs, char *const argv[], char *const env[], int search) {
    struct command c = {0};
    int rc = select_command(file, argv, env, search, &c);
    if (rc) rc = errno;
    else {
        int (*raw_spawn)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const [], char *const []) = dlsym(RTLD_NEXT, "posix_spawn");
        rc = raw_spawn(pid, c.file, actions, attrs, c.argv, c.env);
    }
    dispose(&c); return rc;
}
int posix_spawn(pid_t *pid, const char *p, const posix_spawn_file_actions_t *f, const posix_spawnattr_t *t, char *const a[], char *const e[]) { return spawn_command(pid, p, f, t, a, e, 0); }
int posix_spawnp(pid_t *pid, const char *p, const posix_spawn_file_actions_t *f, const posix_spawnattr_t *t, char *const a[], char *const e[]) { return spawn_command(pid, p, f, t, a, e, 1); }

/* glibc's system() and popen() exec /bin/sh with internal calls past this hook: on Android
 * that ran mksh with Debian's PATH (Python os.system, audit on Tab S10 FE). Keep glibc's
 * implementations (signals, streams, pclose) and hand Android's sh a command that execs
 * Debian's sh through the loader and this hook. */
static void shell_quote(char **out, const char *s) {
    *(*out)++ = '\'';
    for (; *s; ++s) {
        if (*s == '\'') { memcpy(*out, "'\\''", 4); *out += 4; }
        else *(*out)++ = *s;
    }
    *(*out)++ = '\'';
    *(*out)++ = ' ';
}
/* NULL with *failed unset: no native runtime here, run the command as glibc would. */
static char *debian_shell_command(const char *cmd, int *failed) {
    const char *plib = getenv("DD_PLIB"), *lp = getenv("DD_LIBPATH");
    char sh[PATH_MAX], loader[PATH_MAX];
    Dl_info own;
    *failed = 0;
    if (!cmd || !plib || !lp) return NULL;
    *failed = 1;
    if (!dladdr((void *)debian_shell_command, &own) || !own.dli_fname) { errno = ENOENT; return NULL; }
    char *(*raw_realpath)(const char *, char *) = dlsym(RTLD_NEXT, "realpath");
    char *hook = raw_realpath(own.dli_fname, NULL);
    const char *shell = mapped("/bin/sh", sh, 0);
    if (!hook || !shell || snprintf(loader, sizeof loader, "%s/ld.so", plib) >= (int)sizeof loader) {
        free(hook); return NULL;
    }
    const char *parts[] = {loader, "--library-path", lp, "--preload", hook, "--argv0", "sh", shell, "-c", "--", cmd};
    size_t size = sizeof "exec ";
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) size += strlen(parts[i]) * 4 + 3;
    char *line = malloc(size), *out = line;
    if (!line) errno = ENOMEM;
    else {
        *failed = 0;
        memcpy(out, "exec ", 5); out += 5;
        for (size_t i = 0; i < sizeof parts / sizeof parts[0]; ++i) shell_quote(&out, parts[i]);
        out[-1] = 0;
    }
    free(hook);
    return line;
}
int system(const char *cmd) {
    REAL(system, int, const char *);
    int failed;
    char *line = debian_shell_command(cmd, &failed);
    if (failed) return -1;
    int rc = real_system(line ? line : cmd);
    int e = errno; free(line); errno = e;
    return rc;
}
FILE *popen(const char *cmd, const char *mode) {
    REAL(popen, FILE *, const char *, const char *);
    int failed;
    char *line = debian_shell_command(cmd, &failed);
    if (failed) return NULL;
    FILE *f = real_popen(line ? line : cmd, mode);
    int e = errno; free(line); errno = e;
    return f;
}

/* mkstemp and relatives open with internal calls: /tmp was Android's, so EACCES. The name
 * keeps the caller's Debian path; open and mkdir below go through the hook. */
static int make_temp(char *name, int suffix, int flags, int directory) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t len = name ? strlen(name) : 0;
    if (suffix < 0 || len < 6 + (size_t)suffix || memcmp(name + len - suffix - 6, "XXXXXX", 6)) {
        errno = EINVAL; return -1;
    }
    char *x = name + len - suffix - 6;
    int saved = errno;
    for (int attempt = 0; attempt < 238328; ++attempt) {
        unsigned char r[6];
        if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint64_t v = (uint64_t)now.tv_nsec ^ ((uint64_t)getpid() << 20) ^ (uint64_t)attempt * 7919;
            for (int i = 0; i < 6; ++i) { r[i] = (unsigned char)v; v >>= 8; }
        }
        for (int i = 0; i < 6; ++i) x[i] = letters[r[i] % 62];
        int fd = directory ? mkdir(name, 0700)
                           : open(name, (flags & ~O_ACCMODE) | O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { errno = saved; return fd; }
        if (errno != EEXIST) return -1;
    }
    errno = EEXIST; return -1;
}
int mkstemp(char *t) { return make_temp(t, 0, 0, 0); }
int mkstemp64(char *t) { return make_temp(t, 0, 0, 0); }
int mkostemp(char *t, int f) { return make_temp(t, 0, f, 0); }
int mkostemp64(char *t, int f) { return make_temp(t, 0, f, 0); }
int mkstemps(char *t, int s) { return make_temp(t, s, 0, 0); }
int mkstemps64(char *t, int s) { return make_temp(t, s, 0, 0); }
int mkostemps(char *t, int s, int f) { return make_temp(t, s, f, 0); }
int mkostemps64(char *t, int s, int f) { return make_temp(t, s, f, 0); }
char *mkdtemp(char *t) { return make_temp(t, 0, 0, 1) ? NULL : t; }
FILE *tmpfile(void) {
    char name[] = "/tmp/tmpfXXXXXX";
    int fd = make_temp(name, 0, 0, 0);
    if (fd < 0) return NULL;
    unlink(name);
    FILE *f = fdopen(fd, "w+");
    if (!f) { int e = errno; close(fd); errno = e; }
    return f;
}
FILE *tmpfile64(void) { return tmpfile(); }

/* execl and execle build argv and call execve internally. */
static int exec_list(const char *path, const char *arg, va_list ap, int with_env) {
    va_list count;
    va_copy(count, ap);
    size_t n = 1;
    while (va_arg(count, const char *)) ++n;
    va_end(count);
    const char **argv = calloc(n + 1, sizeof *argv);
    if (!argv) { errno = ENOMEM; return -1; }
    argv[0] = arg;
    for (size_t i = 1; i < n; ++i) argv[i] = va_arg(ap, const char *);
    (void)va_arg(ap, const char *);
    char *const *env = with_env ? va_arg(ap, char *const *) : environ;
    int rc = execute(path, (char *const *)argv, env, 0);
    int e = errno; free(argv); errno = e;
    return rc;
}
int execl(const char *path, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    int rc = exec_list(path, arg, ap, 0);
    va_end(ap); return rc;
}
int execle(const char *path, const char *arg, ...) {
    va_list ap; va_start(ap, arg);
    int rc = exec_list(path, arg, ap, 1);
    va_end(ap); return rc;
}

/* Path functions glibc implements without the wrapped entry points (audit, Tab S10 FE). */
int creat(const char *p, mode_t m) { return open(p, O_CREAT | O_WRONLY | O_TRUNC, m); }
int creat64(const char *p, mode_t m) { return open(p, O_CREAT | O_WRONLY | O_TRUNC, m); }
int mkfifo(const char *p, mode_t m) { REAL(mkfifo, int, const char *, mode_t); return real_mkfifo(map(p, 1), m); }
int mkfifoat(int d, const char *p, mode_t m) { REAL(mkfifoat, int, int, const char *, mode_t); return real_mkfifoat(d, mapd(d, p, 1), m); }
int mknod(const char *p, mode_t m, dev_t v) { REAL(mknod, int, const char *, mode_t, dev_t); return real_mknod(map(p, 1), m, v); }
int mknodat(int d, const char *p, mode_t m, dev_t v) { REAL(mknodat, int, int, const char *, mode_t, dev_t); return real_mknodat(d, mapd(d, p, 1), m, v); }
int utime(const char *p, const struct utimbuf *t) { REAL(utime, int, const char *, const struct utimbuf *); return real_utime(map(p, 0), t); }
int lutimes(const char *p, const struct timeval t[2]) { REAL(lutimes, int, const char *, const struct timeval *); return real_lutimes(map(p, 1), t); }
long pathconf(const char *p, int n) { REAL(pathconf, long, const char *, int); return real_pathconf(map(p, 0), n); }
FILE *freopen(const char *p, const char *m, FILE *f) { REAL(freopen, FILE *, const char *, const char *, FILE *); return real_freopen(p ? map(p, 0) : NULL, m, f); }
FILE *freopen64(const char *p, const char *m, FILE *f) { REAL(freopen64, FILE *, const char *, const char *, FILE *); return real_freopen64(p ? map(p, 0) : NULL, m, f); }
/* fs.watch (vite, nodemon, tsc --watch) watched Android paths. */
int inotify_add_watch(int fd, const char *p, uint32_t m) { REAL(inotify_add_watch, int, int, const char *, uint32_t); return real_inotify_add_watch(fd, map(p, (m & IN_DONT_FOLLOW) != 0), m); }
ssize_t getxattr(const char *p, const char *n, void *v, size_t s) { REAL(getxattr, ssize_t, const char *, const char *, void *, size_t); return real_getxattr(map(p, 0), n, v, s); }
ssize_t lgetxattr(const char *p, const char *n, void *v, size_t s) { REAL(lgetxattr, ssize_t, const char *, const char *, void *, size_t); return real_lgetxattr(map(p, 1), n, v, s); }
int setxattr(const char *p, const char *n, const void *v, size_t s, int f) { REAL(setxattr, int, const char *, const char *, const void *, size_t, int); return real_setxattr(map(p, 0), n, v, s, f); }
int lsetxattr(const char *p, const char *n, const void *v, size_t s, int f) { REAL(lsetxattr, int, const char *, const char *, const void *, size_t, int); return real_lsetxattr(map(p, 1), n, v, s, f); }
ssize_t listxattr(const char *p, char *l, size_t s) { REAL(listxattr, ssize_t, const char *, char *, size_t); return real_listxattr(map(p, 0), l, s); }
ssize_t llistxattr(const char *p, char *l, size_t s) { REAL(llistxattr, ssize_t, const char *, char *, size_t); return real_llistxattr(map(p, 1), l, s); }
int removexattr(const char *p, const char *n) { REAL(removexattr, int, const char *, const char *); return real_removexattr(map(p, 0), n); }
int lremovexattr(const char *p, const char *n) { REAL(lremovexattr, int, const char *, const char *); return real_lremovexattr(map(p, 1), n); }
