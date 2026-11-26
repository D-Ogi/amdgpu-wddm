/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The C runtime of the stage TPM: the freestanding C library that libtpms and OpenSSL need (design: docs/design/
 * tpm-preos.md, "The parts"). It has no threads and no file system. The headers in include/ declare the standard
 * functions; this header is the interface of the runtime itself, for the engine (tpm/engine) and the host test.
 *
 * - Memory: malloc, calloc, realloc and free use one arena that the caller gives to crt_init. The runtime never asks
 *   the firmware for memory. The allocator is deterministic: the same calls in the same order give the same
 *   addresses.
 * - Time: every clock of <time.h> is the monotonic clock of crt_init, in milliseconds.
 * - Failure: abort(), exit() and a failed assert() write one log line and return to the last crt_guard_enter point
 *   with crt_longjmp. There is no process to end.
 * - Log: text for stdout, stderr, file descriptors 1 and 2, and the runtime's own messages goes to the sink of
 *   crt_set_log, or nowhere when there is no sink.
 */
#ifndef CRT_CRT_H
#define CRT_CRT_H
#include <stddef.h>

typedef unsigned long long (*crt_clock_fn)(void *ctx);
typedef void (*crt_log_fn)(void *ctx, const char *text, size_t bytes);

/* Starts the runtime on arena[0..bytes). Returns 1, or 0 when the arena is smaller than CRT_ARENA_MIN or the clock is
 * NULL. A second call starts a new arena: every block of the old arena is then gone. */
#define CRT_ARENA_MIN 4096u
int crt_init(void *arena, size_t bytes, crt_clock_fn monotonic_ms, void *ctx);

/* The arena counters. high_water is the largest extent of the arena that was ever in use (the arena size that this
 * load needs); peak_in_use is the largest sum of live blocks, with their headers. */
struct crt_arena_stats {
	size_t size;		/* usable bytes of the arena */
	size_t in_use;		/* bytes of the live blocks now, with headers */
	size_t peak_in_use;
	size_t high_water;
	size_t live_blocks;
	unsigned long long allocations;	/* malloc, calloc and realloc calls that returned a block */
	unsigned long long failures;	/* calls that returned NULL because the arena was full */
};
void crt_arena_stats(struct crt_arena_stats *out);

/* Writes zeros over the whole arena and stops the allocator: every later malloc returns NULL. For the last step of
 * the owner, so that no key material stays in memory that the next owner gets. */
void crt_arena_wipe(void);

/* The monotonic clock of crt_init, in milliseconds. 0 before crt_init. */
unsigned long long crt_monotonic_ms(void);

/* The log sink. NULL: no log. */
void crt_set_log(crt_log_fn sink, void *ctx);
/* One line of the runtime's own, with a prefix. */
void crt_log_text(const char *text);

/* setjmp and longjmp for the x64 calling convention: rbx, rbp, rdi, rsi, rsp, r12-r15, the return address and
 * xmm6-xmm15. No unwinding: no frame between the two calls may need it (C code only). */
typedef struct crt_jmp_buf {
	unsigned long long slot[30];
	struct crt_jmp_buf *previous;	/* the guard below this one (crt_guard_enter) */
} crt_jmp_buf;
__attribute__((returns_twice)) int crt_setjmp(crt_jmp_buf *buffer);
_Noreturn void crt_longjmp(crt_jmp_buf *buffer, int value);

/* The guard of one call into the engine. crt_guard_enter makes buffer the target of abort(); crt_guard_leave restores
 * the previous target. A failure without a guard stops the processor in a loop: there is nowhere to return to.
 *
 *	crt_jmp_buf jb;
 *	if (crt_setjmp(&jb)) { crt_guard_leave(&jb); return failure; }
 *	crt_guard_enter(&jb);
 *	...
 *	crt_guard_leave(&jb);
 */
void crt_guard_enter(crt_jmp_buf *buffer);
void crt_guard_leave(crt_jmp_buf *buffer);
/* The number of failures (abort, exit, assert) since crt_init. */
unsigned crt_failures(void);

/* abort() with a reason in the log. */
_Noreturn void crt_fail(const char *reason);

#endif
