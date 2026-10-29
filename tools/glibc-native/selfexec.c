// Re-exec of the program itself, as bun does for its embedded ripgrep ("rg" as argv0 through
// /proc/self/exe). Started through the patched loader, /proc/self/exe is the loader; pathhook
// must hand out and re-execute the real program. argv[1] of the first run: "proc" or "loader".
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

int main(int argc, char **argv) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) { perror("readlink"); return 1; }
    exe[n] = 0;
    if (!strcmp(argv[0], "rg")) {
        printf("selfexec child %s %s\n", argc > 1 ? argv[1] : "-", strstr(exe, "/usr/bin/selfexec") ? "exe-ok" : exe);
        return 0;
    }
    if (!strstr(exe, "/usr/bin/selfexec")) { fprintf(stderr, "readlink gave %s\n", exe); return 1; }
    char *child[] = {"rg", "--no-config", NULL};
    if (argc > 1 && !strcmp(argv[1], "loader")) {
        char loader[PATH_MAX];
        snprintf(loader, sizeof loader, "%s/ld.so", getenv("DD_PLIB"));
        execv(loader, child);
    } else {
        execv("/proc/self/exe", child);
    }
    perror("execv");
    return 1;
}
