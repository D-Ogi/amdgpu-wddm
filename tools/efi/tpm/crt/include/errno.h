/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <errno.h> of the stage C runtime (crt.h). One errno: the runtime has no threads. The values are those of Linux,
 * so that a value in a log line reads the same as in the Windows core. */
#ifndef CRT_ERRNO_H
#define CRT_ERRNO_H

extern int crt_errno;
#define errno crt_errno

#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENFILE 23
#define EMFILE 24
#define ENOSPC 28
#define ESPIPE 29
#define EROFS 30
#define EDOM 33
#define ERANGE 34
#define ENOSYS 38
#define EOVERFLOW 75
#define EWOULDBLOCK EAGAIN

#endif
