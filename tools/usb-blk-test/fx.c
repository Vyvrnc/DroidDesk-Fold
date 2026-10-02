/* libdroiddesk_blk.so edge cases: dup refused, a forked child cannot use the parent's
 * session, and a write left pending at exit (no close) still reaches the disk. */
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int fd = open("/dev/droiddesk-blk", O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    printf("dup -> %d (want -1)\n", dup(fd));
    fflush(stdout);
    pid_t p = fork();
    if (p == 0) {
        char b[512];
        printf("child read -> %zd (want -1)\n", read(fd, b, 512));
        fflush(stdout);
        _exit(0);
    }
    waitpid(p, 0, 0);
    char b[512];
    printf("parent read -> %zd (want 512)\n", read(fd, b, 512));
    lseek(fd, 512, SEEK_SET);
    if (write(fd, "Z", 1) != 1) perror("write");
    return 0; /* no close: the shim's destructor must flush */
}
