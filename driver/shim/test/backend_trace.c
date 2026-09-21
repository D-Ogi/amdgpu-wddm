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

#define MAX_ALIASES  4u

struct reg_alias {
	u32 alias;
	u32 target;
};

static struct reg_alias g_alias[MAX_ALIASES];
static unsigned int g_alias_count;

#define MAX_REACTIONS  4u

/* A declared hardware reaction: one register write that makes the hardware change a second
 * register, where the second change is the hardware's doing and not the driver's. See
 * backend_add_reaction() in the header for the one pair this test declares and why. */
struct reg_reaction {
	u32 trigger;            /* byte offset written */
	u32 mask;               /* reaction happens when the written value has any of these bits */
	u32 cond;               /* and only while this register, under cond_mask, reads cond_value */
	u32 cond_mask;          /* 0 means no condition */
	u32 cond_value;
	u32 target;             /* byte offset the hardware changes */
	u32 value;              /* what it becomes */
};

static struct reg_reaction g_reaction[MAX_REACTIONS];
static unsigned int g_reaction_count;

#define MAX_SELFCLEAR  4u

/* Bits that do not stay where the driver put them. See backend_add_selfclear() in the header. */
struct reg_selfclear {
	u32 offset;
	u32 mask;
};

static struct reg_selfclear g_selfclear[MAX_SELFCLEAR];
static unsigned int g_selfclear_count;

/* Told about every register write, for a model that needs more than this file's own three can
 * express. backend_mem.c's MEC fetch state is the only user; see backend_set_write_hook(). */
static backend_write_hook g_write_hook;

void backend_set_write_hook(backend_write_hook hook)
{
	g_write_hook = hook;
}

int backend_add_alias(u32 alias_offset, u32 target_offset)
{
	if (g_alias_count >= MAX_ALIASES)
		return -1;
	g_alias[g_alias_count].alias = alias_offset;
	g_alias[g_alias_count].target = target_offset;
	g_alias_count++;
	return 0;
}

void backend_clear_aliases(void)
{
	g_alias_count = 0;
	g_reaction_count = 0;
	g_selfclear_count = 0;
}

int backend_add_selfclear(u32 byte_offset, u32 mask)
{
	if (g_selfclear_count >= MAX_SELFCLEAR)
		return -1;
	g_selfclear[g_selfclear_count].offset = byte_offset;
	g_selfclear[g_selfclear_count].mask = mask;
	g_selfclear_count++;
	return 0;
}

int backend_add_reaction(u32 trigger_offset, u32 trigger_mask,
			 u32 cond_offset, u32 cond_mask, u32 cond_value,
			 u32 target_offset, u32 target_value)
{
	if (g_reaction_count >= MAX_REACTIONS)
		return -1;
	g_reaction[g_reaction_count].trigger = trigger_offset;
	g_reaction[g_reaction_count].mask = trigger_mask;
	g_reaction[g_reaction_count].cond = cond_offset;
	g_reaction[g_reaction_count].cond_mask = cond_mask;
	g_reaction[g_reaction_count].cond_value = cond_value;
	g_reaction[g_reaction_count].target = target_offset;
	g_reaction[g_reaction_count].value = target_value;
	g_reaction_count++;
	return 0;
}

/*
 * Read a whole sweep log into a NUL-terminated ASCII buffer, converting if it is UTF-16LE.
 *
 * The Linux sweeps of experiment E03 are plain ASCII. The Windows ones of E10 come out of a
 * PowerShell redirection and are UTF-16LE with a byte-order mark, and they are read here too.
 * Rather than making the caller care, the encoding is detected from the first two bytes. Only the
 * low byte of each unit is kept: every character in these files is a register name, a hex digit or
 * punctuation, so anything that does not fit in 7 bits is already corruption and the parser below
 * will reject the line.
 *
 * Returns a malloc'd buffer the caller frees, or NULL.
 */
static char *read_sweep_file(const char *path)
{
	long size;
	size_t got, i;
	unsigned char *raw;
	char *out;
	FILE *f = fopen(path, "rb");

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	size = ftell(f);
	rewind(f);
	if (size <= 0) {
		fclose(f);
		return NULL;
	}

	raw = (unsigned char *)malloc((size_t)size);
	if (raw == NULL) {
		fclose(f);
		return NULL;
	}
	got = fread(raw, 1, (size_t)size, f);
	fclose(f);

	if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
		/* UTF-16LE, byte-order mark included. */
		out = (char *)malloc(got / 2 + 1);
		if (out == NULL) {
			free(raw);
			return NULL;
		}
		for (i = 2; i + 1 < got; i += 2)
			out[(i - 2) / 2] = (char)raw[i];
		out[(got - 2) / 2] = '\0';
	} else {
		out = (char *)malloc(got + 1);
		if (out == NULL) {
			free(raw);
			return NULL;
		}
		memcpy(out, raw, got);
		out[got] = '\0';
	}

	free(raw);
	return out;
}

/* One `NAME 0xOFFSET VALUE` line of a sweep buffer into the register state. `replace` makes a later
 * file win over an earlier one, which is what the Windows-state pass needs; the default is first
 * occurrence wins, which is what loading run1 then run2 needs. */
static int load_sweep_buffer(char *buf, int replace)
{
	char *line, *next;
	int added = 0;

	for (line = buf; line != NULL && *line != '\0'; line = next) {
		char name[256];
		unsigned int off = 0, val = 0;

		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';

		if (line[0] == '#')
			continue;
		if (sscanf(line, "%255s 0x%x %x", name, &off, &val) != 3)
			continue;               /* SKIPPED, blank, and anything else unparsable */
		if ((off & 3u) != 0 || off >= BAR5_BYTES)
			continue;
		if (!replace && g_from_sweep[off / 4u])
			continue;
		g_sweep_value[off / 4u] = val;
		g_value[off / 4u] = val;
		g_known[off / 4u] = 1;
		g_from_sweep[off / 4u] = 1;
		added++;
	}
	return added;
}

int backend_load_sweep_replace(const char *path)
{
	char *buf = read_sweep_file(path);
	int added;

	if (buf == NULL)
		return -1;
	added = load_sweep_buffer(buf, 1);
	free(buf);
	return added;
}

int backend_load_sweep(const char *path)
{
	char *buf = read_sweep_file(path);
	int added;

	if (buf == NULL)
		return -1;
	added = load_sweep_buffer(buf, 0);
	free(buf);
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

void backend_poke(u32 byte_offset, u32 value)
{
	if ((byte_offset & 3u) != 0 || byte_offset >= BAR5_BYTES)
		return;
	g_value[byte_offset / 4u] = value;
	g_known[byte_offset / 4u] = 1;
	/* Also into the sweep layer, so backend_reset_state() keeps it for the next run: a seeded
	 * read value describes unit A at the start of the window, and every run of the window starts
	 * there. The CP stub's pokes land here too, which is harmless - they only ever set
	 * SCRATCH_REG0, which the next run overwrites with 0xCAFEDEAD before reading it. */
	g_sweep_value[byte_offset / 4u] = value;
	g_from_sweep[byte_offset / 4u] = 1;
}

int backend_seed_reads(const char *path)
{
	char line[512];
	int added = 0;
	FILE *f = fopen(path, "r");
	static unsigned char seen[BAR5_DWORDS];

	if (f == NULL)
		return -1;

	/* Lines of tools/trace/extract_phase.py --reads --no-fold:
	 *     "  0.549693  R  GC.GE_FAST_CLKS   0x08920  00000000"  */
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		double t;
		char kind[8], name[256];
		unsigned int off = 0, val = 0;

		if (sscanf(line, "%lf %7s %255s 0x%x %x", &t, kind, name, &off, &val) != 5)
			continue;
		if (kind[0] != 'R')
			continue;
		if ((off & 3u) != 0 || off >= BAR5_BYTES)
			continue;
		if (seen[off / 4u])
			continue;               /* first read of this offset wins */
		seen[off / 4u] = 1;
		backend_poke(off, val);
		added++;
	}
	fclose(f);
	return added;
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

/* --- the touched-register survey ------------------------------------------------------------ */

#define MAX_TOUCHED  1024u

static struct {
	u32 offset;
	int wrote;
} g_touched[MAX_TOUCHED];
static unsigned int g_touched_count;
static int g_touched_on;
static unsigned int g_touched_overflow;

static void touch(u32 byte_offset, int wrote)
{
	unsigned int i;

	if (!g_touched_on)
		return;
	for (i = 0; i < g_touched_count; i++) {
		if (g_touched[i].offset == byte_offset) {
			g_touched[i].wrote |= wrote;
			return;
		}
	}
	if (g_touched_count >= MAX_TOUCHED) {
		g_touched_overflow++;
		return;
	}
	g_touched[g_touched_count].offset = byte_offset;
	g_touched[g_touched_count].wrote = wrote;
	g_touched_count++;
}

void backend_touched_start(void)
{
	g_touched_count = 0;
	g_touched_overflow = 0;
	g_touched_on = 1;
}

void backend_touched_stop(void)
{
	g_touched_on = 0;
}

unsigned int backend_touched_count(void)
{
	return g_touched_count;
}

unsigned int backend_touched_overflow(void)
{
	return g_touched_overflow;
}

u32 backend_touched_offset(unsigned int i)
{
	return i < g_touched_count ? g_touched[i].offset : 0;
}

int backend_touched_written(unsigned int i)
{
	return i < g_touched_count ? g_touched[i].wrote : 0;
}

/* --- the two functions every shim backend provides ------------------------------------------ */

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index)
{
	(void)adev;

	if (dword_index >= BAR5_DWORDS) {
		fprintf(stderr, "backend: read outside BAR5, dword index 0x%X\n", dword_index);
		exit(2);
	}
	touch(dword_index * 4u, 0);
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
	unsigned int i;

	(void)adev;

	if (dword_index >= BAR5_DWORDS) {
		fprintf(stderr, "backend: write outside BAR5, dword index 0x%X\n", dword_index);
		exit(2);
	}
	if (g_write_count >= MAX_WRITES) {
		fprintf(stderr, "backend: more than %u writes recorded, raise MAX_WRITES\n", MAX_WRITES);
		exit(2);
	}
	touch(dword_index * 4u, 1);
	g_writes[g_write_count].byte_offset = dword_index * 4u;
	g_writes[g_write_count].value = value;
	g_write_count++;
	g_value[dword_index] = value;
	g_known[dword_index] = 1;

	/* A declared hardware aliasing, applied to the register state but never recorded as a write:
	 * the comparison must see exactly what the code under test asked for. */
	for (i = 0; i < g_alias_count; i++) {
		if (g_alias[i].alias == dword_index * 4u) {
			g_value[g_alias[i].target / 4u] = value;
			g_known[g_alias[i].target / 4u] = 1;
		}
	}

	/* Same rule for a declared reaction: the hardware's own change, never recorded as a write.
	 * The condition is what makes it a model of the hardware rather than a wish - the engine that
	 * would perform the reaction has to be running for it to happen. */
	for (i = 0; i < g_reaction_count; i++) {
		u32 c = g_reaction[i].cond / 4u;

		if (g_reaction[i].trigger != dword_index * 4u || (value & g_reaction[i].mask) == 0)
			continue;
		if (g_reaction[i].cond_mask != 0 &&
		    (!g_known[c] || (g_value[c] & g_reaction[i].cond_mask) != g_reaction[i].cond_value))
			continue;
		g_value[g_reaction[i].target / 4u] = g_reaction[i].value;
		g_known[g_reaction[i].target / 4u] = 1;
	}

	/* The fourth declared model, and the only one that is not about a register: the MEC's own
	 * copy of a queue's base and read pointer, which survives a halt. It observes rather than
	 * reacts, and it lives in backend_mem.c, because a stale fetch ends in a ring that executes
	 * nothing and an interrupt vector - both of which are that file's. See backend_mem.h for what
	 * was measured. It is reached through a hook rather than by name so that this file stays
	 * linkable on its own: replay.c uses it without backend_mem.c at all. */
	if (g_write_hook != NULL)
		g_write_hook(dword_index * 4u, value);

	/* Bits the hardware drops on the way in. The RECORDED write above is what the driver asked
	 * for, so the comparison with the trace's W line is untouched; only the state a later read
	 * sees is corrected, which is what the trace's R line shows. */
	for (i = 0; i < g_selfclear_count; i++) {
		if (g_selfclear[i].offset == dword_index * 4u)
			g_value[dword_index] &= ~g_selfclear[i].mask;
	}
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
