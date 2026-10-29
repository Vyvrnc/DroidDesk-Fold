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
extern char **environ;

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
    int fd = (int)syscall(SYS_openat, AT_FDCWD, "/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char buf[8192];
    ssize_t n = 0, r;
    while (n < (ssize_t)sizeof buf - 1 && (r = read(fd, buf + n, sizeof buf - 1 - (size_t)n)) > 0) n += r;
    close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *a = buf, *end = buf + n;
    if (!is_loader(a)) return;
    for (a += strlen(a) + 1; a < end; a += strlen(a) + 1) {
        if (ld_option_with_value(a)) { a += strlen(a) + 1; continue; }
        if (!strcmp(a, "--")) { a += strlen(a) + 1; break; }
        if (ld_option(a)) continue;
        break;
    }
    if (a < end && *a && strlen(a) < sizeof self_exe) strcpy(self_exe, a);
}
static int is_proc_self_exe(const char *p) {
    if (!p) return 0;
    if (!strcmp(p, "/proc/self/exe")) return 1;
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
/* Caller-owned stack buffers are private to each thread and nested invocation. */
static const char *mapped(const char *p, char b[PATH_MAX], int nofollow) {
    if (!p || p[0] != '/') return p;
    const char *root = getenv("DD_ROOTFS"), *tmp = getenv("DD_TMP");
    if (!root || (tmp && pre(p, tmp))) return p;
    if (tmp && (pre(p, "/tmp") || pre(p, "/dev/shm"))) {
        if (snprintf(b, PATH_MAX, "%s%s", tmp, p + (p[1] == 't' ? 4 : 8)) >= PATH_MAX)
            goto toolong;
        return b;
    }
    /* Physical rootfs paths occur in HOME and in the initial exec command. */
    if (pre(p, root)) p += strlen(root);
    else if (!system_path(p)) return p;
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
            found = !syscall(SYS_newfstatat, AT_FDCWD, b, &st, AT_SYMLINK_NOFOLLOW);
            /* A missing final component is valid for creation; a missing parent is not. */
            if (!found && *rest) return NULL;
        }
        if (found && S_ISLNK(st.st_mode)) {
            if (++links > 40) { errno = ELOOP; return NULL; }
            ssize_t len = syscall(SYS_readlinkat, AT_FDCWD, b, target, sizeof(target) - 1);
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
    }
    return p;
}
#define REAL(name, ret, ...) \
    static ret (*cache_##name)(__VA_ARGS__); \
    ret (*real_##name)(__VA_ARGS__) = __atomic_load_n(&cache_##name, __ATOMIC_ACQUIRE); \
    if (!real_##name) { real_##name = dlsym(RTLD_NEXT, #name); \
        __atomic_store_n(&cache_##name, real_##name, __ATOMIC_RELEASE); } \
    typedef ret result_type __attribute__((unused)); \
    char pathbuf[PATH_MAX] __attribute__((unused))
#define mapbuf(p, b, f) ({ const char *mp = mapped(p, b, f); \
    if (!mp) { return _Generic((result_type)0, \
        FILE *: NULL, DIR *: NULL, char *: NULL, void *: NULL, default: -1); } mp; })
#define map(p, f) mapbuf(p, pathbuf, f)

static int open_nofollow(int flags) {
    return (flags & O_NOFOLLOW) || (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
}

int open(const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(open, int, const char *, int, ...); return real_open(map(p, open_nofollow(f)), f, m); }
int open64(const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(open64, int, const char *, int, ...); return real_open64(map(p, open_nofollow(f)), f, m); }
int openat(int d, const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(openat, int, int, const char *, int, ...); return real_openat(d, map(p, open_nofollow(f)), f, m); }
int openat64(int d, const char *p, int f, ...) { mode_t m = 0; if ((f & O_CREAT) || (f & O_TMPFILE) == O_TMPFILE) { va_list a; va_start(a, f); m = va_arg(a, int); va_end(a); }
    REAL(openat64, int, int, const char *, int, ...); return real_openat64(d, map(p, open_nofollow(f)), f, m); }
FILE *fopen(const char *p, const char *m) { REAL(fopen, FILE *, const char *, const char *); return real_fopen(map(p, 0), m); }
FILE *fopen64(const char *p, const char *m) { REAL(fopen64, FILE *, const char *, const char *); return real_fopen64(map(p, 0), m); }
int stat(const char *p, struct stat *s) { REAL(stat, int, const char *, struct stat *); return real_stat(map(p, 0), s); }
int lstat(const char *p, struct stat *s) { REAL(lstat, int, const char *, struct stat *); return real_lstat(map(p, 1), s); }
int stat64(const char *p, struct stat64 *s) { REAL(stat64, int, const char *, struct stat64 *); return real_stat64(map(p, 0), s); }
int lstat64(const char *p, struct stat64 *s) { REAL(lstat64, int, const char *, struct stat64 *); return real_lstat64(map(p, 1), s); }
int fstatat(int d, const char *p, struct stat *s, int f) { REAL(fstatat, int, int, const char *, struct stat *, int); return real_fstatat(d, map(p, f & AT_SYMLINK_NOFOLLOW), s, f); }
int fstatat64(int d, const char *p, struct stat64 *s, int f) { REAL(fstatat64, int, int, const char *, struct stat64 *, int); return real_fstatat64(d, map(p, f & AT_SYMLINK_NOFOLLOW), s, f); }
int statx(int d, const char *p, int f, unsigned m, struct statx *s) { REAL(statx, int, int, const char *, int, unsigned, struct statx *); return real_statx(d, map(p, f & AT_SYMLINK_NOFOLLOW), f, m, s); }
int access(const char *p, int m) { REAL(access, int, const char *, int); return real_access(map(p, 0), m); }
int faccessat(int d, const char *p, int m, int f) { REAL(faccessat, int, int, const char *, int, int); return real_faccessat(d, map(p, f & AT_SYMLINK_NOFOLLOW), m, f); }
DIR *opendir(const char *p) { REAL(opendir, DIR *, const char *); return real_opendir(map(p, 0)); }
ssize_t readlink(const char *p, char *b, size_t n) { if (*self_exe && is_proc_self_exe(p)) return copy_self_exe(b, n);
    REAL(readlink, ssize_t, const char *, char *, size_t); return real_readlink(map(p, 1), b, n); }
ssize_t readlinkat(int d, const char *p, char *b, size_t n) { if (*self_exe && is_proc_self_exe(p)) return copy_self_exe(b, n);
    REAL(readlinkat, ssize_t, int, const char *, char *, size_t); return real_readlinkat(d, map(p, 1), b, n); }
char *realpath(const char *p, char *r) { REAL(realpath, char *, const char *, char *); return unmap(real_realpath(map(p, 0), r)); }
int mkdir(const char *p, mode_t m) { REAL(mkdir, int, const char *, mode_t); return real_mkdir(map(p, 1), m); }
int unlink(const char *p) { REAL(unlink, int, const char *); return real_unlink(map(p, 1)); }
int rename(const char *a, const char *b) { REAL(rename, int, const char *, const char *); char second[PATH_MAX]; const char *ma = map(a, 1); return real_rename(ma, mapbuf(b, second, 1)); }
int __open_2(const char *p, int f) { REAL(__open_2, int, const char *, int); return real___open_2(map(p, open_nofollow(f)), f); }
int __open64_2(const char *p, int f) { REAL(__open64_2, int, const char *, int); return real___open64_2(map(p, open_nofollow(f)), f); }
int __openat_2(int d, const char *p, int f) { REAL(__openat_2, int, int, const char *, int); return real___openat_2(d, map(p, open_nofollow(f)), f); }
int __openat64_2(int d, const char *p, int f) { REAL(__openat64_2, int, int, const char *, int); return real___openat64_2(d, map(p, open_nofollow(f)), f); }
int scandir(const char *p, struct dirent ***l, int (*s)(const struct dirent *), int (*c)(const struct dirent **, const struct dirent **)) {
    REAL(scandir, int, const char *, struct dirent ***, int (*)(const struct dirent *), int (*)(const struct dirent **, const struct dirent **)); return real_scandir(map(p, 0), l, s, c); }
int scandir64(const char *p, struct dirent64 ***l, int (*s)(const struct dirent64 *), int (*c)(const struct dirent64 **, const struct dirent64 **)) {
    REAL(scandir64, int, const char *, struct dirent64 ***, int (*)(const struct dirent64 *), int (*)(const struct dirent64 **, const struct dirent64 **)); return real_scandir64(map(p, 0), l, s, c); }
void *dlopen(const char *p, int f) { REAL(dlopen, void *, const char *, int); return real_dlopen(p ? map(p, 0) : NULL, f); }

int chdir(const char *p) { REAL(chdir, int, const char *); return real_chdir(map(p, 0)); }
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
int mkdirat(int d, const char *p, mode_t m) { REAL(mkdirat, int, int, const char *, mode_t); return real_mkdirat(d, map(p, 1), m); }
int unlinkat(int d, const char *p, int f) { REAL(unlinkat, int, int, const char *, int); return real_unlinkat(d, map(p, 1), f); }
int rmdir(const char *p) { REAL(rmdir, int, const char *); return real_rmdir(map(p, 1)); }
int chmod(const char *p, mode_t m) { REAL(chmod, int, const char *, mode_t); return real_chmod(map(p, 0), m); }
int fchmodat(int d, const char *p, mode_t m, int f) { REAL(fchmodat, int, int, const char *, mode_t, int); return real_fchmodat(d, map(p, f & AT_SYMLINK_NOFOLLOW), m, f); }
int chown(const char *p, uid_t u, gid_t g) { REAL(chown, int, const char *, uid_t, gid_t); return real_chown(map(p, 0), u, g); }
int lchown(const char *p, uid_t u, gid_t g) { REAL(lchown, int, const char *, uid_t, gid_t); return real_lchown(map(p, 1), u, g); }
int fchownat(int d, const char *p, uid_t u, gid_t g, int f) { REAL(fchownat, int, int, const char *, uid_t, gid_t, int); return real_fchownat(d, map(p, f & AT_SYMLINK_NOFOLLOW), u, g, f); }
int utimensat(int d, const char *p, const struct timespec t[2], int f) { REAL(utimensat, int, int, const char *, const struct timespec *, int); return real_utimensat(d, map(p, f & AT_SYMLINK_NOFOLLOW), t, f); }
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
    return real_symlinkat(map(a, 1), d, mapbuf(b, second, 1));
}
int link(const char *a, const char *b) {
    REAL(link, int, const char *, const char *); char second[PATH_MAX];
    return real_link(map(a, 1), mapbuf(b, second, 1));
}
int linkat(int a, const char *p, int b, const char *q, int f) {
    REAL(linkat, int, int, const char *, int, const char *, int); char second[PATH_MAX];
    return real_linkat(a, map(p, !(f & AT_SYMLINK_FOLLOW)), b, mapbuf(q, second, 1), f);
}
int renameat(int a, const char *p, int b, const char *q) {
    REAL(renameat, int, int, const char *, int, const char *); char second[PATH_MAX];
    return real_renameat(a, map(p, 1), b, mapbuf(q, second, 1));
}
int renameat2(int a, const char *p, int b, const char *q, unsigned f) {
    REAL(renameat2, int, int, const char *, int, const char *, unsigned); char second[PATH_MAX];
    return real_renameat2(a, map(p, 1), b, mapbuf(q, second, 1), f);
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
    const char *runtime[] = {"DD_ROOTFS", "DD_TMP", "DD_PLIB", "DD_LIBPATH",
        "DD_SAVED_LD_PRELOAD", "DD_SAVED_LD_LIBRARY_PATH", "DD_SAVED_PATH", NULL};
    size_t n = count(env), k = 0;
    size_t saved = 0;
    for (char **e = environ; *e; ++e) if (!strncmp(*e, "DD_SAVED_", 9)) ++saved;
    char **v = calloc(n + 12 + 2 * saved, sizeof(*v));
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
static int prepare(const char *file, char *const argv[], char *const env[], struct command *c, int depth) {
    if (depth > 8) { errno = ELOOP; return -1; }
    if (*self_exe && (is_proc_self_exe(file) ||
                      (is_loader(file) && !(count(argv) > 1 && ld_option(argv[1])))))
        file = self_exe;
    char path[PATH_MAX], interp[PATH_MAX], script[256];
    const char *target = mapped(file, path, 0);
    if (!target) return -1;
    if (access(target, X_OK)) return -1;
    int kind = inspect(target, interp, script);
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
