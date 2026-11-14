/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <sys/stat.h> of the stage C runtime (crt.h). There is no file system: stat and fstat fail with ENOENT. */
#ifndef CRT_SYS_STAT_H
#define CRT_SYS_STAT_H
#include <sys/types.h>

struct stat {
	mode_t st_mode;
	off_t st_size;
	time_t st_mtime;
};

#define S_IFMT 0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_IRUSR 0400
#define S_IWUSR 0200

int stat(const char *path, struct stat *out);
int fstat(int fd, struct stat *out);

#endif
