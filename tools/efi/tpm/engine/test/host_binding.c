/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The host binding of the engine test: a Windows console program around the objects of tpm_engine.lib. It has no
 * C library of the host (lld-link /nodefaultlib): the engine's own C runtime formats the text, and this file calls
 * kernel32 and bcrypt directly. The declarations are those of the Windows SDK (processthreadsapi.h, memoryapi.h,
 * profileapi.h, bcrypt.h), written out here because the build has no SDK headers.
 *
 * The test runs on a thread whose stack is committed in full at creation (no guard page): the engine objects are
 * built for the stage without stack probes, and a frame larger than one page would otherwise skip the guard page.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "test_host.h"

typedef unsigned long DWORD;
#define DECL __declspec(dllimport)
DECL void *__stdcall GetStdHandle(DWORD id);
DECL int __stdcall WriteFile(void *file, const void *buffer, DWORD bytes, DWORD *written, void *overlapped);
DECL void *__stdcall VirtualAlloc(void *address, size_t bytes, DWORD type, DWORD protect);
DECL int __stdcall QueryPerformanceCounter(long long *count);
DECL int __stdcall QueryPerformanceFrequency(long long *frequency);
DECL void *__stdcall CreateThread(void *security, size_t stack_bytes, DWORD(__stdcall *start)(void *), void *arg,
				  DWORD flags, DWORD *id);
DECL DWORD __stdcall WaitForSingleObject(void *handle, DWORD ms);
DECL int __stdcall GetExitCodeThread(void *thread, DWORD *code);
DECL const char *__stdcall GetCommandLineA(void);
DECL __attribute__((noreturn)) void __stdcall ExitProcess(unsigned code);
DECL long __stdcall BCryptGenRandom(void *algorithm, unsigned char *buffer, DWORD bytes, DWORD flags);

#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define PAGE_READWRITE 0x04u
#define INFINITE 0xFFFFFFFFu
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x2u

#define ARENA_BYTES (16u << 20)
#define STACK_BYTES (2u << 20)
#define PAINT_BYTES (STACK_BYTES - (256u << 10))	/* the paint stays clear of the top of the stack */
#define PAINT 0xA5u

static void *g_out;
static long long g_frequency;
static struct tpm_engine_platform g_platform;
static unsigned char *g_paint_top;	/* the highest painted address + 1 */

void host_print(const char *format, ...)
{
	char line[1024];
	va_list args;
	int n;
	DWORD written;
	va_start(args, format);
	n = vsnprintf(line, sizeof(line) - 1, format, args);
	va_end(args);
	if (n < 0) return;
	if ((size_t)n > sizeof(line) - 2) n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	WriteFile(g_out, line, (DWORD)n, &written, NULL);
}

unsigned long long host_us(void)
{
	long long c;
	QueryPerformanceCounter(&c);
	return (unsigned long long)(c / g_frequency) * 1000000ull + (unsigned long long)(c % g_frequency) * 1000000ull / (unsigned long long)g_frequency;
}

static unsigned long long host_ms(void *ctx)
{
	(void)ctx;
	return host_us() / 1000;
}

static int host_entropy(void *ctx, unsigned char *out, size_t n)
{
	(void)ctx;
	return n <= 0xFFFFFFFFu && BCryptGenRandom(NULL, out, (DWORD)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

static void host_log(void *ctx, const char *text, size_t bytes)
{
	DWORD written;
	(void)ctx;
	WriteFile(g_out, "  log: ", 7, &written, NULL);
	WriteFile(g_out, text, (DWORD)bytes, &written, NULL);
	if (bytes && text[bytes - 1] != '\n') WriteFile(g_out, "\n", 1, &written, NULL);
}

const struct tpm_engine_platform *host_platform(void) { return &g_platform; }

/* The paint starts 4 KiB below the frame of this function, so that its own frame and the memset frame stay above. */
__attribute__((noinline)) void host_stack_paint(void)
{
	volatile unsigned char here = 0;
	unsigned char *top = (unsigned char *)&here - 4096;
	g_paint_top = top;
	memset(top - PAINT_BYTES, PAINT, PAINT_BYTES);
}

size_t host_stack_used(void)
{
	const unsigned char *p = g_paint_top - PAINT_BYTES;
	while (p < g_paint_top && *p == PAINT) p++;
	/* + the 4 KiB gap above the paint: an upper bound of the use below the frame of host_stack_paint */
	return (size_t)(g_paint_top - p) + 4096;
}

static DWORD __stdcall test_thread(void *arg)
{
	return (DWORD)engine_test_run((const char *)arg);
}

/* The value of "--mutate=NAME" on the command line, or NULL. */
static const char *mutation_argument(void)
{
	static char name[64];
	const char *at = strstr(GetCommandLineA(), "--mutate=");
	size_t n = 0;
	if (!at) return NULL;
	at += 9;
	while (at[n] && at[n] != ' ' && at[n] != '"' && n < sizeof(name) - 1) {
		name[n] = at[n];
		n++;
	}
	name[n] = 0;
	return name;
}

void host_entry(void)
{
	void *thread;
	DWORD code = 99;

	g_out = GetStdHandle(STD_OUTPUT_HANDLE);
	QueryPerformanceFrequency(&g_frequency);
	g_platform.arena = VirtualAlloc(NULL, ARENA_BYTES, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	g_platform.arena_bytes = ARENA_BYTES;
	g_platform.monotonic_ms = host_ms;
	g_platform.entropy = host_entropy;
	g_platform.ctx = NULL;
	if (!g_platform.arena) {
		host_print("FAIL VirtualAlloc of the arena");
		ExitProcess(2);
	}
	tpm_engine_set_log(host_log, NULL);
	/* flags 0: STACK_BYTES is the commit size of the new stack, not only its reservation */
	thread = CreateThread(NULL, STACK_BYTES, test_thread, (void *)mutation_argument(), 0, NULL);
	if (!thread) {
		host_print("FAIL CreateThread");
		ExitProcess(2);
	}
	WaitForSingleObject(thread, INFINITE);
	GetExitCodeThread(thread, &code);
	ExitProcess((unsigned)code);
}
