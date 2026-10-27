#pragma once
#define _GNU_SOURCE
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>
static inline int ASharedMemory_create(const char *name, size_t size) {
    int fd = memfd_create(name, 0);
    if (fd >= 0 && ftruncate(fd, (off_t)size) < 0) return -1;
    return fd;
}
static inline int ASharedMemory_setProt(int fd, int prot) { (void)fd; (void)prot; return 0; }
