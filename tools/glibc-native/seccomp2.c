// Android-like seccomp: kcmp through the generic syscall() kills the process, hard links fail
// with EPERM. With pathhook preloaded, syscall(kcmp) must return ENOSYS and link() must fall
// back to a copy that survives unlinking the source (npm's cacache), refuse an existing name
// and copy a symlink as a symlink. Argument: a writable directory. Prints "seccomp2 ok".
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
    if (lstat(dst, &st) || !S_ISREG(st.st_mode)) { fprintf(stderr, "link did not become a file\n"); return 1; }
    if (unlink(src)) { perror("unlink"); return 1; }
    char buf[8] = {0};
    f = fopen(dst, "r");
    if (!f || !fgets(buf, sizeof buf, f) || strcmp(buf, "x")) { fprintf(stderr, "copy lost its content\n"); return 1; }
    fclose(f);
    if (link(dst, dst) != -1 || errno != EEXIST) { fprintf(stderr, "existing name: errno=%d\n", errno); return 1; }
    char sym[512], sym2[512], target[64];
    snprintf(sym, sizeof sym, "%s/link-sym", argv[1]);
    snprintf(sym2, sizeof sym2, "%s/link-sym2", argv[1]);
    unlink(sym); unlink(sym2);
    if (symlink("some-target", sym) || linkat(AT_FDCWD, sym, AT_FDCWD, sym2, 0)) { perror("symlink link"); return 1; }
    ssize_t n = readlink(sym2, target, sizeof target - 1);
    if (n < 0 || (target[n] = 0, strcmp(target, "some-target"))) { fprintf(stderr, "symlink was not copied\n"); return 1; }
    puts("seccomp2 ok");
    return 0;
}
