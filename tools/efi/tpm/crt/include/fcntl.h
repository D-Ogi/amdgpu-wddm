/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <fcntl.h> of the stage C runtime (crt.h). There is no file system: open fails with ENOENT. */
#ifndef CRT_FCNTL_H
#define CRT_FCNTL_H
#include <sys/types.h>

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_EXCL 0200
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000
#define O_BINARY 0

int open(const char *path, int flags, ...);

#endif
