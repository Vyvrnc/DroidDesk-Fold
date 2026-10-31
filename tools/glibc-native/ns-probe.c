// Probe: may an Android app create a user + mount namespace and bind-mount a directory over a
// system path? That would let raw-syscall programs (Bun, Go, static) see Debian paths.
// Run inside the app (native shell), not from adb. Argument: a directory to bind over /mnt.
// Each step prints OK or FAIL with errno; nothing outside the private namespace changes.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static void report(const char *what, int ok) {
    printf("%-40s %s", what, ok ? "OK" : "FAIL");
    if (!ok) printf(" (%s)", strerror(errno));
    printf("\n");
    fflush(stdout);
}
static int write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *src = argc > 1 ? argv[1] : NULL;
    uid_t uid = getuid(); gid_t gid = getgid();
    FILE *f = fopen("/proc/sys/user/max_user_namespaces", "r");
    if (f) { int n = -1; if (fscanf(f, "%d", &n) == 1) printf("max_user_namespaces = %d\n", n); fclose(f); }
    else printf("max_user_namespaces: not readable (%s)\n", strerror(errno));
    pid_t pid = fork();
    if (pid == 0) {
        int ok = unshare(CLONE_NEWUSER) == 0;
        report("unshare(CLONE_NEWUSER)", ok);
        if (!ok) _exit(1);
        char map[64];
        snprintf(map, sizeof map, "0 %u 1", uid);
        report("write /proc/self/setgroups deny", write_file("/proc/self/setgroups", "deny") == 0);
        report("write uid_map", write_file("/proc/self/uid_map", map) == 0);
        snprintf(map, sizeof map, "0 %u 1", gid);
        report("write gid_map", write_file("/proc/self/gid_map", map) == 0);
        ok = unshare(CLONE_NEWNS) == 0;
        report("unshare(CLONE_NEWNS)", ok);
        if (!ok) _exit(1);
        report("make / private", mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) == 0);
        if (src) {
            ok = mount(src, "/mnt", NULL, MS_BIND | MS_REC, NULL) == 0;
            report("bind mount over /mnt", ok);
            if (ok) {
                struct stat a, b;
                report("/mnt now shows the source", !stat("/mnt", &a) && !stat(src, &b) && a.st_ino == b.st_ino);
            }
            ok = mount("tmpfs", "/mnt", "tmpfs", 0, NULL) == 0;
            report("tmpfs mount over /mnt", ok);
        }
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    printf("child exit=%d signal=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1,
           WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return 0;
}
