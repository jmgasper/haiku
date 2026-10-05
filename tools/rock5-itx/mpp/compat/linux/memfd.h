#ifndef ROCK5_MPP_COMPAT_LINUX_MEMFD_H
#define ROCK5_MPP_COMPAT_LINUX_MEMFD_H
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
#define MFD_CLOEXEC 0x0001U
static inline int
memfd_create(const char* name, unsigned int flags)
{
    static unsigned int serial;
    if (flags & ~MFD_CLOEXEC) {
        errno = EINVAL;
        return -1;
    }
    for (unsigned int attempt = 0; attempt < 32; attempt++) {
        char path[96];
        unsigned int id = __sync_add_and_fetch(&serial, 1);
        snprintf(path, sizeof(path), "/rock5-mpp-%ld-%u-%s",
            (long)getpid(), id, name ? name : "ring");
        int fd = shm_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return -1;
        }
        shm_unlink(path);
        if (flags & MFD_CLOEXEC)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        return fd;
    }
    errno = EEXIST;
    return -1;
}
#endif
