/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <unistd.h> of the stage C runtime (crt.h). File descriptors 1 and 2 write to the log sink; every other descriptor
 * fails with EBADF. */
#ifndef CRT_UNISTD_H
#define CRT_UNISTD_H
#include <sys/types.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define R_OK 4
#define W_OK 2

pid_t getpid(void);
ssize_t write(int fd, const void *buffer, size_t bytes);
ssize_t read(int fd, void *buffer, size_t bytes);
int close(int fd);
int unlink(const char *path);
int access(const char *path, int mode);
off_t lseek(int fd, off_t offset, int whence);
int fsync(int fd);
int ftruncate(int fd, off_t length);
/* Busy waits on the monotonic clock: the stage has no scheduler. */
unsigned sleep(unsigned seconds);
int usleep(unsigned long long microseconds);

#endif
