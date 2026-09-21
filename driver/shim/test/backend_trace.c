/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host replay backend for driver/shim.
 *
 * Reads are answered from unit A's own register state: the pre-driver read sweeps of experiment
 * E03 (evidence/linux/2026-09-21-E03-init-trace/sweep-before-run*.log), overlaid with whatever the
 * code under test has written since. Writes are recorded in order and nothing else happens; no
 * hardware is touched and no register value is invented. A read of an offset that neither sweep
 * covered is counted and reported, because an invented read value would silently invalidate every
 * register the imported code derives from it.
 *
 * This is one of the two backends behind bc250_shim_rreg / bc250_shim_wreg (see bc250_shim.h).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_trace.h"

/* BAR5 of 1002:13FE is 512 KB (facts M14), so every offset the SOC15 macros can produce fits. */
#define BAR5_BYTES   0x80000u
#define BAR5_DWORDS  (BAR5_BYTES / 4u)

#define MAX_WRITES   4096u

static u32 g_value[BAR5_DWORDS];                 /* what a read returns now */
static unsigned char g_known[BAR5_DWORDS];       /* 1 = a sweep or a write gave this offset a value */
static u32 g_sweep_value[BAR5_DWORDS];           /* unit A's firmware state, kept so a rerun can go back */
static unsigned char g_from_sweep[BAR5_DWORDS];  /* 1 = a sweep log had this offset */

static struct bc250_reg_write g_writes[MAX_WRITES];
static unsigned int g_write_count;

static unsigned int g_unknown_reads;
static u32 g_first_unknown_read;
static int g_verbose;

int backend_load_sweep(const char *path)
{
	char line[512];
	int added = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;

	/* Format, one register per line (tools/diagusb sweep_stream):
	 *     BLOCK.NAME 0xOFFSET VALUE
	 * with `#` comments and the literal token SKIPPED where the sweep refused to read. The last
	 * line of run1 is truncated (the machine hung mid-read), so short lines are dropped too. */
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		char name[256];
		unsigned int off = 0, val = 0;
		size_t len = strlen(line);

		if (len == 0 || line[len - 1] != '\n')
			continue;                       /* truncated last line: not a finished reading */
		if (line[0] == '#')
			continue;
		if (sscanf(line, "%255s 0x%x %x", name, &off, &val) != 3)
			continue;                       /* SKIPPED and anything else unparsable */
		if ((off & 3u) != 0 || off >= BAR5_BYTES)
			continue;
		if (g_from_sweep[off / 4u])
			continue;                       /* first occurrence wins */
		g_sweep_value[off / 4u] = val;
		g_value[off / 4u] = val;
		g_known[off / 4u] = 1;
		g_from_sweep[off / 4u] = 1;
		added++;
	}
	fclose(f);
	return added;
}

void backend_reset_state(void)
{
	unsigned int i;

	/* Put every register back to what the sweep read on unit A, including the ones the previous
	 * run wrote over. Without this a second run would be reading the first run's results, and a
	 * read-modify-write register could agree with the trace for the wrong reason. */
	for (i = 0; i < BAR5_DWORDS; i++) {
		g_value[i] = g_from_sweep[i] ? g_sweep_value[i] : 0;
		g_known[i] = g_from_sweep[i];
	}
	g_unknown_reads = 0;
	g_first_unknown_read = 0;
}

void backend_reset_writes(void)
{
	g_write_count = 0;
}

const struct bc250_reg_write *backend_writes(void)
{
	return g_writes;
}

unsigned int backend_write_count(void)
{
	return g_write_count;
}

unsigned int backend_unknown_reads(void)
{
	return g_unknown_reads;
}

u32 backend_first_unknown_read(void)
{
	return g_first_unknown_read;
}

void backend_set_verbose(int on)
{
	g_verbose = on;
}

/* --- the two functions every shim backend provides ------------------------------------------ */

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index)
{
	(void)adev;

	if (dword_index >= BAR5_DWORDS) {
		fprintf(stderr, "backend: read outside BAR5, dword index 0x%X\n", dword_index);
		exit(2);
	}
	if (!g_known[dword_index]) {
		if (g_unknown_reads == 0)
			g_first_unknown_read = dword_index * 4u;
		g_unknown_reads++;
		return 0;
	}
	return g_value[dword_index];
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value)
{
	(void)adev;

	if (dword_index >= BAR5_DWORDS) {
		fprintf(stderr, "backend: write outside BAR5, dword index 0x%X\n", dword_index);
		exit(2);
	}
	if (g_write_count >= MAX_WRITES) {
		fprintf(stderr, "backend: more than %u writes recorded, raise MAX_WRITES\n", MAX_WRITES);
		exit(2);
	}
	g_writes[g_write_count].byte_offset = dword_index * 4u;
	g_writes[g_write_count].value = value;
	g_write_count++;
	g_value[dword_index] = value;
	g_known[dword_index] = 1;
}

/* Nothing to wait for: the replayed registers change only when the code under test writes them,
 * so a delay could only make the poll loops slower, never change their outcome. */
void bc250_shim_udelay(unsigned int usec)
{
	(void)usec;
}

void bc250_shim_log(int level, void *dev, const char *fmt, ...)
{
	va_list ap;
	static const char *const tag[] = { "info", "warn", "err " };

	(void)dev;
	if (!g_verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "[amdgpu %s] ", tag[level < 0 ? 0 : (level > 2 ? 2 : level)]);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
