// Android-like seccomp: kcmp through the generic syscall() kills the process, hard links fail
// with EPERM. With pathhook preloaded, syscall(kcmp) must return ENOSYS and link() must fall
// back to a symlink. Argument: a writable directory. Prints "seccomp2 ok" on success.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_kcmp, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_linkat, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#ifdef __NR_link
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_link, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {sizeof code / sizeof code[0], code};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
        perror("seccomp");
        return 1;
    }
    long rc = syscall(__NR_kcmp, getpid(), getpid(), 0, 0, 0);
    if (rc != -1 || errno != ENOSYS) { fprintf(stderr, "kcmp: rc=%ld errno=%d\n", rc, errno); return 1; }
    char src[512], dst[512];
    snprintf(src, sizeof src, "%s/link-src", argv[1]);
    snprintf(dst, sizeof dst, "%s/link-dst", argv[1]);
    FILE *f = fopen(src, "w");
    if (!f) { perror("fopen"); return 1; }
    fputs("x", f);
    fclose(f);
    unlink(dst);
    if (link(src, dst)) { perror("link"); return 1; }
    struct stat st;
    if (lstat(dst, &st) || !S_ISLNK(st.st_mode)) { fprintf(stderr, "link did not become a symlink\n"); return 1; }
    puts("seccomp2 ok");
    return 0;
}
