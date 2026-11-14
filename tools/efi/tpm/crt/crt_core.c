/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The start, the clock, the log, the guard and the failure path of the stage C runtime (crt.h). */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "crt.h"
#include "crt_internal.h"

int crt_errno;

static crt_clock_fn g_clock;
static void *g_clock_ctx;
static crt_log_fn g_log;
static void *g_log_ctx;
static crt_jmp_buf *g_guard;
static unsigned g_failures;

void errno_set_nomem(void) { crt_errno = ENOMEM; }

int crt_init(void *arena, size_t bytes, crt_clock_fn monotonic_ms, void *ctx)
{
	if (!monotonic_ms || !crt_alloc_init(arena, bytes)) return 0;
	g_clock = monotonic_ms;
	g_clock_ctx = ctx;
	g_guard = NULL;
	g_failures = 0;
	crt_errno = 0;
	return 1;
}

unsigned long long crt_monotonic_ms(void)
{
	return g_clock ? g_clock(g_clock_ctx) : 0;
}

void crt_set_log(crt_log_fn sink, void *ctx)
{
	g_log = sink;
	g_log_ctx = ctx;
}

void crt_log_write(const char *text, size_t bytes)
{
	if (g_log && bytes) g_log(g_log_ctx, text, bytes);
}

int crt_log_vformat(const char *format, va_list args)
{
	char line[512];
	int n = vsnprintf(line, sizeof(line), format, args);
	if (n < 0) return n;
	crt_log_write(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
	return n;
}

void crt_log_text(const char *text)
{
	static const char prefix[] = "crt: ";
	crt_log_write(prefix, sizeof(prefix) - 1);
	crt_log_write(text, strlen(text));
	crt_log_write("\n", 1);
}

void crt_guard_enter(crt_jmp_buf *buffer)
{
	buffer->previous = g_guard;
	g_guard = buffer;
}

void crt_guard_leave(crt_jmp_buf *buffer)
{
	if (g_guard == buffer) g_guard = buffer->previous;
}

unsigned crt_failures(void) { return g_failures; }

_Noreturn void crt_fail(const char *reason)
{
	crt_jmp_buf *target = g_guard;
	g_failures++;
	crt_log_text(reason);
	if (target) crt_longjmp(target, 1);
	/* no guard: there is no caller to return to */
	for (;;) __asm__ volatile("pause");
}

_Noreturn void abort(void) { crt_fail("abort"); }

_Noreturn void exit(int status)
{
	(void)status;
	crt_fail("exit");
}

_Noreturn void crt_assert_fail(const char *expression, const char *file, int line)
{
	char text[256];
	snprintf(text, sizeof(text), "assertion failed: %s (%s:%d)", expression, file, line);
	crt_fail(text);
}
