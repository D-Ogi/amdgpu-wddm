/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <stdio.h> of the stage C runtime (crt.h). There is no file system: fopen always fails, and text for stdout or
 * stderr goes to the log sink of crt_set_log. */
#ifndef CRT_STDIO_H
#define CRT_STDIO_H
#include <stdarg.h>
#include <stddef.h>

typedef struct crt_file FILE;
typedef long long fpos_t;

extern FILE *const crt_stdin;
extern FILE *const crt_stdout;
extern FILE *const crt_stderr;
#define stdin crt_stdin
#define stdout crt_stdout
#define stderr crt_stderr

#define EOF (-1)
#define BUFSIZ 512
#define FILENAME_MAX 260
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int vsnprintf(char *out, size_t capacity, const char *format, va_list args);
int snprintf(char *out, size_t capacity, const char *format, ...);
int vsprintf(char *out, const char *format, va_list args);
int sprintf(char *out, const char *format, ...);
int vasprintf(char **out, const char *format, va_list args);
int asprintf(char **out, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list args);
int fprintf(FILE *stream, const char *format, ...);
int vprintf(const char *format, va_list args);
int printf(const char *format, ...);
int vdprintf(int fd, const char *format, va_list args);
int dprintf(int fd, const char *format, ...);
int fputs(const char *s, FILE *stream);
int puts(const char *s);
int fputc(int c, FILE *stream);
int putc(int c, FILE *stream);
int putchar(int c);
void perror(const char *s);
void setbuf(FILE *stream, char *buffer);

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fflush(FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fileno(FILE *stream);
int remove(const char *path);
int rename(const char *from, const char *to);
int sscanf(const char *s, const char *format, ...);

#endif
