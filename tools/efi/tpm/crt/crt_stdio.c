/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * <stdio.h>, <unistd.h>, <fcntl.h> and <sys/stat.h> of the stage C runtime. The stage TPM has no files: its NV
 * state goes through the callbacks of libtpms into memory (tpm/engine). The file functions are here because code
 * that the engine never calls refers to them (the file NV backend of libtpms in tpm_nvfile.c, the random seed file
 * and the file store of OpenSSL). They fail as a C library fails on a missing file: fopen and open give ENOENT.
 *
 * stdout, stderr and the descriptors 1 and 2 write to the log sink (crt_set_log). stdin reads nothing.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "crt_internal.h"

struct crt_file {
	int fd;
};

static struct crt_file g_streams[3] = {{0}, {1}, {2}};
FILE *const crt_stdin = &g_streams[0];
FILE *const crt_stdout = &g_streams[1];
FILE *const crt_stderr = &g_streams[2];

static int is_log(const FILE *stream) { return stream == crt_stdout || stream == crt_stderr; }

int vfprintf(FILE *stream, const char *format, va_list args)
{
	if (!is_log(stream)) {
		crt_errno = EBADF;
		return -1;
	}
	return crt_log_vformat(format, args);
}

int fprintf(FILE *stream, const char *format, ...)
{
	va_list args;
	int n;
	va_start(args, format);
	n = vfprintf(stream, format, args);
	va_end(args);
	return n;
}

int vprintf(const char *format, va_list args) { return vfprintf(stdout, format, args); }

int printf(const char *format, ...)
{
	va_list args;
	int n;
	va_start(args, format);
	n = vfprintf(stdout, format, args);
	va_end(args);
	return n;
}

int vdprintf(int fd, const char *format, va_list args)
{
	if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
		crt_errno = EBADF;
		return -1;
	}
	return crt_log_vformat(format, args);
}

int dprintf(int fd, const char *format, ...)
{
	va_list args;
	int n;
	va_start(args, format);
	n = vdprintf(fd, format, args);
	va_end(args);
	return n;
}

int fputs(const char *s, FILE *stream)
{
	if (!is_log(stream)) return EOF;
	crt_log_write(s, strlen(s));
	return 0;
}

int puts(const char *s)
{
	fputs(s, stdout);
	crt_log_write("\n", 1);
	return 0;
}

int fputc(int c, FILE *stream)
{
	char ch = (char)c;
	if (!is_log(stream)) return EOF;
	crt_log_write(&ch, 1);
	return (unsigned char)c;
}

int putc(int c, FILE *stream) { return fputc(c, stream); }
int putchar(int c) { return fputc(c, stdout); }

void perror(const char *s)
{
	if (s && *s) fprintf(stderr, "%s: error %d\n", s, crt_errno);
	else fprintf(stderr, "error %d\n", crt_errno);
}

void setbuf(FILE *stream, char *buffer)
{
	(void)stream;
	(void)buffer;
}

FILE *fopen(const char *path, const char *mode)
{
	(void)path;
	(void)mode;
	crt_errno = ENOENT;
	return NULL;
}

FILE *fdopen(int fd, const char *mode)
{
	(void)fd;
	(void)mode;
	crt_errno = EBADF;
	return NULL;
}

int fclose(FILE *stream) { return is_log(stream) || stream == crt_stdin ? 0 : EOF; }

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
	(void)buffer;
	(void)size;
	(void)count;
	(void)stream;
	return 0;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
	if (!is_log(stream) || !size) return 0;
	crt_log_write(buffer, size * count);
	return count;
}

int fflush(FILE *stream)
{
	(void)stream;
	return 0;
}

int fseek(FILE *stream, long offset, int whence)
{
	(void)stream;
	(void)offset;
	(void)whence;
	crt_errno = ESPIPE;
	return -1;
}

long ftell(FILE *stream)
{
	(void)stream;
	crt_errno = ESPIPE;
	return -1;
}

char *fgets(char *s, int size, FILE *stream)
{
	(void)s;
	(void)size;
	(void)stream;
	return NULL;
}

int fgetc(FILE *stream)
{
	(void)stream;
	return EOF;
}

int getc(FILE *stream) { return fgetc(stream); }
int feof(FILE *stream) { return stream == crt_stdin; }
int ferror(FILE *stream) { return !is_log(stream) && stream != crt_stdin; }
void clearerr(FILE *stream) { (void)stream; }
int fileno(FILE *stream) { return stream->fd; }

int remove(const char *path)
{
	(void)path;
	crt_errno = ENOENT;
	return -1;
}

int rename(const char *from, const char *to)
{
	(void)from;
	(void)to;
	crt_errno = ENOENT;
	return -1;
}

pid_t getpid(void) { return 1; }

ssize_t write(int fd, const void *buffer, size_t bytes)
{
	if (fd != STDOUT_FILENO && fd != STDERR_FILENO) {
		crt_errno = EBADF;
		return -1;
	}
	crt_log_write(buffer, bytes);
	return (ssize_t)bytes;
}

ssize_t read(int fd, void *buffer, size_t bytes)
{
	(void)buffer;
	(void)bytes;
	if (fd == STDIN_FILENO) return 0;
	crt_errno = EBADF;
	return -1;
}

int close(int fd)
{
	if (fd >= STDIN_FILENO && fd <= STDERR_FILENO) return 0;
	crt_errno = EBADF;
	return -1;
}

int unlink(const char *path) { return remove(path); }

int access(const char *path, int mode)
{
	(void)path;
	(void)mode;
	crt_errno = ENOENT;
	return -1;
}

off_t lseek(int fd, off_t offset, int whence)
{
	(void)fd;
	(void)offset;
	(void)whence;
	crt_errno = ESPIPE;
	return -1;
}

int fsync(int fd)
{
	(void)fd;
	crt_errno = EBADF;
	return -1;
}

int ftruncate(int fd, off_t length)
{
	(void)fd;
	(void)length;
	crt_errno = EBADF;
	return -1;
}

int open(const char *path, int flags, ...)
{
	(void)path;
	(void)flags;
	crt_errno = ENOENT;
	return -1;
}

int stat(const char *path, struct stat *out)
{
	(void)path;
	(void)out;
	crt_errno = ENOENT;
	return -1;
}

int fstat(int fd, struct stat *out)
{
	(void)fd;
	(void)out;
	crt_errno = EBADF;
	return -1;
}
