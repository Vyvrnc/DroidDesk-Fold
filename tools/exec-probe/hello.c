// hello-glibc / hello-bionic / libhello.so for the exec probe.
//   (no args)                     prints hello
//   spawn LOADER LIBPATH PROGRAM  starts PROGRAM through LOADER (the loader-in-nativeLibraryDir path)
//   spawn-direct PROGRAM          execs PROGRAM directly (expected to fail at targetSdk >= 29)
#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifdef __BIONIC__
#define FLAVOR "bionic"
#else
#define FLAVOR "glibc"
#endif

int main(int argc, char **argv) {
    pid_t pid;
    int rc, status = 0;
    if (argc > 4 && !strcmp(argv[1], "spawn")) {
        char *args[] = {argv[2], "--library-path", argv[3], argv[4], NULL};
        rc = posix_spawn(&pid, argv[2], NULL, NULL, args, environ);
    } else if (argc > 2 && !strcmp(argv[1], "spawn-direct")) {
        char *args[] = {argv[2], NULL};
        rc = posix_spawn(&pid, argv[2], NULL, NULL, args, environ);
    } else {
        printf("hello %s\n", FLAVOR);
        return 0;
    }
    if (rc) { printf("spawn failed: %s\n", strerror(rc)); return 1; }
    waitpid(pid, &status, 0);
    printf("child exit=%d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 0;
}
