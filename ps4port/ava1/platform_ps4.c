/* PS4 platform layer for the pinned AVA1 C node.
 *
 * The PS4 kernel is FreeBSD 9 based. Its libc exports sysctl(KERN_ARND),
 * ftruncate, futimes and open(O_DIRECT), but not the three calls the PS5
 * layer uses: posix_fallocate, posix_fadvise and utimensat. This file keeps
 * the five-function AVA1 platform contract and leaves the upstream C node
 * untouched. It is our port glue, not upstream code.
 */
#include "ava1_platform.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef KERN_ARND
#define KERN_ARND 37
#endif

int ava1_platform_random(uint8_t *buf, size_t n)
{
    int mib[2] = { CTL_KERN, KERN_ARND };

    while (n > 0) {
        size_t want = n > 256 ? 256 : n;
        size_t got = want;

        if (sysctl(mib, 2, buf, &got, NULL, 0) != 0 || got == 0) {
            return -1;
        }
        buf += got;
        n -= got;
    }
    return 0;
}

void ava1_platform_sleep_ms(unsigned ms)
{
    usleep((useconds_t)ms * 1000u);
}

int ava1_platform_preallocate(int fd, uint64_t len)
{
    /* FreeBSD 9 has no posix_fallocate here. ftruncate grows the file, but
       does not reserve blocks; ENOSPC can still arrive at write time. This
       is the same fallback platform_posix.c uses where fallocate is absent. */
    return ftruncate(fd, (off_t)len) == 0 ? 0 : errno;
}

void ava1_platform_set_mtime(int fd, const char *path, uint64_t mtime)
{
    struct timeval tv[2];

    (void)path;
    tv[0].tv_sec = (time_t)mtime;
    tv[0].tv_usec = 0;
    tv[1] = tv[0];
    (void)futimes(fd, tv);
}

int ava1_platform_drop_cache(int fd, uint64_t off, uint64_t len,
                             const char **how)
{
    (void)fd;
    (void)off;
    (void)len;
    *how = "no cache-drop call on PS4";
    return ENOTSUP;
}

int ava1_platform_open_direct(const char *path)
{
#ifdef O_DIRECT
    return open(path, O_RDONLY | O_DIRECT | O_NOFOLLOW);
#else
    (void)path;
    errno = ENOTSUP;
    return -1;
#endif
}
