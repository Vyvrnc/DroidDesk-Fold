// accept() under a seccomp filter that kills the accept syscall, as Android's app filter does
// (tmux's server died natively, Tab S10 FE). With pathhook preloaded, accept() must go
// through accept4 and survive. Prints "accept ok" on success.
#define _GNU_SOURCE
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

int main(void) {
    struct sock_filter code[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {sizeof code / sizeof code[0], code};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog)) {
        perror("seccomp");
        return 1;
    }
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path + 1, sizeof a.sun_path - 1, "dd-accept-%d", (int)getpid());
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(a.sun_path + 1));
    int server = socket(AF_UNIX, SOCK_STREAM, 0), client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server < 0 || client < 0 || bind(server, (struct sockaddr *)&a, len) || listen(server, 1) ||
        connect(client, (struct sockaddr *)&a, len)) {
        perror("socket setup");
        return 1;
    }
    int peer = accept(server, NULL, NULL);
    if (peer < 0) { perror("accept"); return 1; }
    puts("accept ok");
    return 0;
}
