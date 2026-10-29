#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL %s: %s\n", what, strerror(errno)); exit(1); }
}
static void restored(const char *key, const char *saved) {
    const char *actual = getenv(key), *expected = getenv(saved);
    check(expected && (expected[0] ? actual && !strcmp(actual, expected) : !actual || !actual[0]), key);
}
static void mapped_file(void) {
    char b[16] = {0}; int fd = open("/etc/native-test", O_RDONLY);
    check(fd >= 0, "open mapped");
    check(read(fd, b, sizeof(b)) == 7 && !strcmp(b, "mapped\n"), "mapped contents");
    close(fd);
}
static void *thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 100; ++i) mapped_file();
    return NULL;
}
static void child_status(pid_t pid) {
    int s; check(waitpid(pid, &s, 0) == pid && WIFEXITED(s) && WEXITSTATUS(s) == 0, "child exit");
}
static void dispatch(int mode, const char *file, const char *arg) {
    pid_t pid = fork(); check(pid >= 0, "fork");
    if (!pid) {
        char *a[] = {(char *)file, "child", (char *)arg, NULL};
        /* Explicit envp must be sanitized too. */
        setenv("LD_PRELOAD", "/missing-glibc-preload", 1);
        setenv("LD_LIBRARY_PATH", "/missing-glibc-libs", 1);
        if (mode == 0) execve(file, a, environ);
        if (mode == 1) execv(file, a);
        if (mode == 2) execvp(file, a);
        if (mode == 3) {
            char *minimal[] = {"PATH=/unused", "LD_PRELOAD=/bad", "LD_LIBRARY_PATH=/bad", NULL};
            execvpe(file, a, minimal);
        }
        perror("exec"); _exit(1);
    }
    child_status(pid);
}
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "app")) {
        char path[PATH_MAX];
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
        check(n > 0, "app executable path"); path[n] = 0;
        char *base = strrchr(path, '/'); check(base != NULL, "app directory");
        check(!strcmp(base + 1, "brave"), "original executable name");
        check((size_t)(base + 1 - path) + sizeof("icudtl.dat") <= sizeof(path), "app data path length");
        strcpy(base + 1, "icudtl.dat");
        FILE *f = fopen(path, "r"); check(f != NULL, "ICU beside executable"); fclose(f);
        check(!strcmp(getenv("LANG"), "C.UTF-8"), "launcher LANG fallback");
        check(getenv("DISPLAY") && getenv("TMPDIR"), "app display and temp");
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "library")) {
        void *lib = dlopen("libnative-alias.so", RTLD_NOW);
        check(lib && *(int *)dlsym(lib, "native_symbol") == 42, "loader library mirror");
        dlclose(lib); return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "android")) {
        check(getenv("PREFIX") && !strcmp(getenv("PREFIX"), getenv("DD_SAVED_PREFIX")), "bionic PREFIX");
        check(getenv("TERMUX_VERSION") && !strcmp(getenv("TERMUX_VERSION"), "0.118"), "bionic TERMUX variable");
        check(getenv("TERMUX__TEST") && !strcmp(getenv("TERMUX__TEST"), "value with spaces"), "bionic TERMUX__ variable");
        restored("PATH", "DD_SAVED_PATH");
        restored("LD_LIBRARY_PATH", "DD_SAVED_LD_LIBRARY_PATH");
        restored("LD_PRELOAD", "DD_SAVED_LD_PRELOAD");
        check(!getenv("GCONV_PATH") && !getenv("FONTCONFIG_SYSROOT"), "bionic glibc data removed");
        check(fcntl(63, F_GETFD) >= 0, "bionic fd63");
        return 0;
    }
    mapped_file();
    if (argc > 1 && !strcmp(argv[1], "child")) {
        check(!getenv("LD_PRELOAD") && !getenv("LD_LIBRARY_PATH"), "glibc environment stripped");
        check(getenv("DD_SAVED_PREFIX") && getenv("DD_SAVED_TERMUX__TEST"), "saved Termux env survives custom envp");
        check(fcntl(63, F_GETFD) >= 0, "fd63 survived exec");
        if (argc > 2 && !strcmp(argv[2], "execvp")) check(!strcmp(argv[0], "probe"), "argv0 preserved");
        if (argc > 2 && !strcmp(argv[2], "patched")) {
            char *a[] = {"self-reexec", "child", "reexecuted", NULL};
            execve("/proc/self/exe", a, environ);
            check(0, "patched self re-exec");
        }
        return 0;
    }
    struct stat s; struct statfs fs;
    check(!stat("/etc/native-test", &s) && s.st_size == 7, "stat");
    check(!statfs("/etc/native-test", &fs), "statfs");
    check(!stat("/etc/absolute-file", &s) && s.st_size == 7, "absolute file link");
    check(!stat("/opt/absolute-dir/native-test", &s) && s.st_size == 7, "absolute directory link");
    check(!stat("/opt/relative-dir/native-test", &s) && s.st_size == 7, "relative directory link");
    check(!lstat("/etc/absolute-file", &s) && S_ISLNK(s.st_mode), "lstat link itself");
    check(!fstatat(AT_FDCWD, "/etc/absolute-file", &s, AT_SYMLINK_NOFOLLOW) && S_ISLNK(s.st_mode), "fstatat nofollow");
    struct statx sx;
    check(!statx(AT_FDCWD, "/etc/absolute-file", AT_SYMLINK_NOFOLLOW, STATX_TYPE, &sx) && S_ISLNK(sx.stx_mode), "statx nofollow");
    char target[PATH_MAX];
    ssize_t size = readlink("/etc/absolute-file", target, sizeof(target) - 1);
    check(size > 0, "readlink absolute link"); target[size] = 0;
    check(!strcmp(target, "/etc/native-test"), "absolute target preserved");
    check(readlinkat(AT_FDCWD, "/etc/absolute-file", target, sizeof(target)) == size, "readlinkat link itself");
    errno = 0;
    check(open("/etc/absolute-file", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP, "open nofollow");
    errno = 0;
    check(open("/etc/absolute-file", O_WRONLY | O_CREAT | O_EXCL, 0600) == -1 && errno == EEXIST, "exclusive open keeps link");
    check(!utimensat(AT_FDCWD, "/etc/absolute-file", NULL, AT_SYMLINK_NOFOLLOW), "utimensat nofollow");
    errno = 0;
    check(stat("/etc/loop-a", &s) == -1 && errno == ELOOP, "symlink loop limit");
    check(!rename("/etc/absolute-file", "/etc/absolute-renamed"), "rename link itself");
    check(!lstat("/etc/absolute-renamed", &s) && S_ISLNK(s.st_mode), "renamed link remains link");
    check(!unlink("/etc/absolute-renamed") && !stat("/etc/native-test", &s), "unlink link keeps target");
    DIR *d = opendir("/etc"); check(d != NULL, "opendir"); closedir(d);
    struct dirent **entries;
    int n = scandir("/etc", &entries, NULL, alphasort); check(n > 0, "scandir");
    for (int i = 0; i < n; ++i) free(entries[i]);
    free(entries);
    check(!chdir("/etc"), "chdir");
    char b[PATH_MAX], small[5];
    check(getcwd(small, sizeof(small)) && !strcmp(small, "/etc"), "logical getcwd small buffer");
    char *cwd = getcwd(NULL, 0); check(cwd && !strcmp(cwd, "/etc"), "allocated getcwd"); free(cwd);
    char *rp = realpath("native-test", NULL);
    check(rp && !strcmp(rp, "/etc/native-test"), "realpath"); free(rp);
    check(!chdir("/"), "return cwd");
    char home[PATH_MAX]; snprintf(home, sizeof(home), "%s/root", getenv("DD_ROOTFS"));
    check(realpath(home, b) && !strcmp(b, home), "physical HOME realpath unchanged");
    check(!chdir(home) && getcwd(b, sizeof(b)) && !strcmp(b, home), "physical HOME cwd unchanged");
    check(!chdir("/"), "return from HOME");
    void *lib = dlopen("/opt/native-test.so", RTLD_NOW);
    check(lib && *(int *)dlsym(lib, "native_symbol") == 42, "dlopen"); dlclose(lib);
    int fd = open("/tmp/first", O_CREAT | O_TRUNC | O_WRONLY, 0600); check(fd >= 0, "tmp map"); close(fd);
    check(!rename("/tmp/first", "/var/renamed"), "rename two buffers");
    check(!symlink("/var/renamed", "/etc/sym"), "symlink two buffers");
    ssize_t len = readlink("/etc/sym", b, sizeof(b) - 1); check(len > 0, "readlink"); b[len] = 0;
    char physical[PATH_MAX]; snprintf(physical, sizeof(physical), "%s/var/renamed", getenv("DD_ROOTFS"));
    check(!strcmp(b, physical), "readlink result unchanged");
    check(!stat("/etc/sym", &s), "symlink resolves");
    check(!link("/var/renamed", "/etc/hard"), "link two buffers");
    check(!linkat(AT_FDCWD, "/etc/hard", AT_FDCWD, "/etc/hard2", 0), "linkat");
    check(!symlinkat("/var/renamed", AT_FDCWD, "/etc/sym2"), "symlinkat");
    check(!renameat(AT_FDCWD, "/etc/hard", AT_FDCWD, "/etc/moved"), "renameat");
    check(!renameat2(AT_FDCWD, "/etc/moved", AT_FDCWD, "/etc/moved2", 0), "renameat2");
    check(!chmod("/etc/moved2", 0640) && !fchmodat(AT_FDCWD, "/etc/moved2", 0600, 0), "chmod");
    check(!chown("/etc/moved2", getuid(), getgid()), "chown");
    check(!lchown("/etc/sym", getuid(), getgid()), "lchown");
    check(!fchownat(AT_FDCWD, "/etc/moved2", getuid(), getgid(), 0), "fchownat");
    check(!utimensat(AT_FDCWD, "/etc/moved2", NULL, 0) && !utimes("/etc/moved2", NULL), "times");
    check(!truncate("/etc/moved2", 3), "truncate");
    check(!mkdirat(AT_FDCWD, "/tmp/dir", 0700) && !rmdir("/tmp/dir"), "mkdirat rmdir");
    check(!unlinkat(AT_FDCWD, "/etc/sym2", 0), "unlinkat");
    fd = openat(AT_FDCWD, "/dev/shm/shm", O_CREAT | O_RDWR, 0600); check(fd >= 0, "shm map"); close(fd);
    check(!access("/tmp/shm", F_OK), "tmp and shm share mapping");
    check(access("/etc-not-mapped/native-test", F_OK) < 0, "mapping boundary");
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i) check(!pthread_create(&threads[i], NULL, thread, NULL), "pthread_create");
    for (int i = 0; i < 4; ++i) pthread_join(threads[i], NULL);
    dispatch(0, "/usr/bin/absolute-probe", "absolute exec link");
    dispatch(0, "/usr/bin/probe", "execve");
    dispatch(1, "/usr/bin/probe", "execv");
    dispatch(2, "probe", "execvp");
    dispatch(3, "probe", "execvpe");
    setenv("PATH", "/opt/search:/usr/bin:/bin", 1);
    dispatch(2, "probe", "PATH directory skipped");
    dispatch(0, "/usr/bin/script", "shebang");
    dispatch(0, "/usr/bin/patched", "patched");
    for (int i = 0; i < 2; ++i) {
        pid_t pid; char *a[] = {"probe", "child", "spawn", NULL};
        int rc = i ? posix_spawnp(&pid, "probe", NULL, NULL, a, environ) : posix_spawn(&pid, "/usr/bin/probe", NULL, NULL, a, environ);
        check(rc == 0, "spawn"); child_status(pid);
    }
    for (int minimal = 0; minimal < 2; ++minimal) {
        pid_t pid = fork(); check(pid >= 0, "bionic fork");
        if (!pid) {
            setenv("GCONV_PATH", "/glibc/gconv", 1); setenv("FONTCONFIG_SYSROOT", "/glibc/fonts", 1);
            char *a[] = {"android-marker", "android", NULL};
            char *e[] = {"PREFIX=/wrong", "TERMUX_VERSION=wrong", "TERMUX__TEST=wrong", NULL};
            execve("/usr/bin/android-marker", a, minimal ? e : environ); _exit(1);
        }
        child_status(pid);
    }
    char *a[] = {"missing", NULL}; errno = 0;
    check(execvp("missing-command", a) == -1 && errno == ENOENT, "failed exec errno");
    puts("pathhook mapping, thread and exec/spawn checks OK");
    return 0;
}
