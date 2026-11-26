/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* <stdlib.h> of the stage C runtime (crt.h). The memory functions use the arena of crt_init. */
#ifndef CRT_STDLIB_H
#define CRT_STDLIB_H
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
/* rand() is not a source of random numbers here; see crt_stdlib.c. */
#define RAND_MAX 0x7fff

void *malloc(size_t bytes);
void *calloc(size_t count, size_t bytes);
void *realloc(void *block, size_t bytes);
void free(void *block);

/* The C runtime has no process: abort and exit leave through the guard of the engine call (crt_guard). */
_Noreturn void abort(void);
_Noreturn void exit(int status);

char *getenv(const char *name);
char *secure_getenv(const char *name);

int rand(void);
void srand(unsigned seed);

long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
int atoi(const char *s);
long atol(const char *s);

int abs(int value);
long labs(long value);
long long llabs(long long value);

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
	      int (*compare)(const void *, const void *));

#endif
