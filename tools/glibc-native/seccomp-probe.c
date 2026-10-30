// Probe what an Android app may do with seccomp, for a supervisor that would map paths of
// programs the hook cannot reach (static Go, musl). Run inside the app (native Debian or
// Termux), not from adb shell: only app processes carry the app seccomp filter.
// Prints one line per capability: OK or FAIL with errno.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

static void report(const char *what, int ok) {
    printf("%-34s %s", what, ok ? "OK" : "FAIL");
    if (!ok) printf(" (%s)", strerror(errno));
    printf("\n");
    fflush(stdout);
}

static int install(unsigned action, unsigned flags) {
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_getppid, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, action),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {sizeof code / sizeof code[0], code};
    return (int)syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, flags, &prog);
}

static volatile sig_atomic_t trapped;
static void on_sigsys(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)ctx;
    trapped = info->si_syscall == __NR_getppid;
}

static int send_fd(int sock, int fd) {
    char buf[CMSG_SPACE(sizeof(int))] = {0}, byte = 0;
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof buf};
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) == 1 ? 0 : -1;
}
static int recv_fd(int sock) {
    char buf[CMSG_SPACE(sizeof(int))] = {0}, byte;
    struct iovec iov = {&byte, 1};
    struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof buf};
    if (recvmsg(sock, &msg, 0) != 1) return -1;
    int fd; memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&msg)), sizeof(int)); return fd;
}

int main(void) {
    report("prctl(PR_SET_NO_NEW_PRIVS)", !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0));

    /* 1) In-process trap: SIGSYS handler emulates the call. */
    pid_t pid = fork();
    if (!pid) {
        struct sigaction sa = {0};
        sa.sa_sigaction = on_sigsys; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSYS, &sa, NULL);
        if (install(SECCOMP_RET_TRAP, 0)) _exit(2);
        syscall(__NR_getppid);
        _exit(trapped ? 0 : 3);
    }
    int st = 0; waitpid(pid, &st, 0);
    errno = WIFEXITED(st) && WEXITSTATUS(st) == 2 ? EPERM : 0;
    report("SECCOMP_RET_TRAP + SIGSYS handler", WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 2) User notification: the child installs a filter, hands the listener to the parent,
     * which answers getppid with 4242 and, if supported, injects an fd (ADDFD). */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { report("socketpair", 0); return 1; }
    pid = fork();
    if (!pid) {
        close(sv[0]);
        int listener = install(SECCOMP_RET_USER_NOTIF, SECCOMP_FILTER_FLAG_NEW_LISTENER);
        if (listener < 0) { int e = errno; (void)write(sv[1], &e, sizeof e); _exit(2); }
        int zero = 0; (void)write(sv[1], &zero, sizeof zero);
        send_fd(sv[1], listener);
        close(listener);
        long r = syscall(__NR_getppid);
        _exit(r == 4242 ? 0 : 3);
    }
    close(sv[1]);
    int child_errno = -1;
    if (read(sv[0], &child_errno, sizeof child_errno) != sizeof child_errno || child_errno) {
        errno = child_errno > 0 ? child_errno : EIO;
        report("SECCOMP_FILTER_FLAG_NEW_LISTENER", 0);
        waitpid(pid, &st, 0);
        return 0;
    }
    report("SECCOMP_FILTER_FLAG_NEW_LISTENER", 1);
    int listener = recv_fd(sv[0]);
    struct seccomp_notif req; struct seccomp_notif_resp resp;
    memset(&req, 0, sizeof req);
    int got = ioctl(listener, SECCOMP_IOCTL_NOTIF_RECV, &req) == 0;
    report("SECCOMP_IOCTL_NOTIF_RECV", got);
    if (got) {
        /* Reading the target's memory is what path mapping needs (argument strings). */
        char probe[16]; struct iovec local = {probe, sizeof probe}, remote = {(void *)req.data.instruction_pointer, sizeof probe};
        report("process_vm_readv on target", process_vm_readv(req.pid, &local, 1, &remote, 1, 0) == (ssize_t)sizeof probe);
        char mem[64]; snprintf(mem, sizeof mem, "/proc/%d/mem", req.pid);
        int memfd = open(mem, O_RDWR);
        report("open /proc/PID/mem read-write", memfd >= 0);
        if (memfd >= 0) close(memfd);
#ifdef SECCOMP_IOCTL_NOTIF_ADDFD
        struct seccomp_notif_addfd addfd = {.id = req.id, .srcfd = 1};
        int injected = ioctl(listener, SECCOMP_IOCTL_NOTIF_ADDFD, &addfd);
        report("SECCOMP_IOCTL_NOTIF_ADDFD", injected >= 0);
#endif
        memset(&resp, 0, sizeof resp);
        resp.id = req.id; resp.val = 4242;
        report("SECCOMP_IOCTL_NOTIF_SEND", ioctl(listener, SECCOMP_IOCTL_NOTIF_SEND, &resp) == 0);
#ifdef SECCOMP_USER_NOTIF_FLAG_CONTINUE
        /* CONTINUE lets the kernel run the original call; useful for unmapped paths. */
        report("USER_NOTIF_FLAG_CONTINUE defined", 1);
#endif
    }
    waitpid(pid, &st, 0);
    errno = 0;
    report("target saw the emulated result", WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return 0;
}
