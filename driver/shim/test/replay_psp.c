/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host test of the PSP path (driver/shim/bc250_psp.c over the imported psp_v11_0_8.c).
 *
 *   replay_psp <sweep-run1.log> <sweep-run2.log> <trace-extract.txt> <firmware dir> [-v]
 *
 * The trace extract is amdgpu's own PSP mailbox traffic on unit A (E03), reads included:
 *   python tools/trace/extract_phase.py <evidence>/amdgpu-events.txt \
 *          --match "MP0\.MP0_SMN_C2PMSG_(6|7)" --reads --until 0.309
 * The firmware directory holds linux-firmware's amdgpu/cyan_skillfish2_*.bin (kept outside the
 * repository, P:\BC-250\ref\linux-firmware\amdgpu, see PROVENANCE.txt there).
 *
 * What is real and what is a model:
 *   - Register reads start from unit A's firmware state (the two pre-driver sweeps of E03).
 *   - The PSP is a model, and a deliberately strict one: it answers a ring create the way unit A's
 *     PSP answered amdgpu (C2PMSG_64: 0x80000000 -> 0x80020000), and on every write pointer update
 *     it reads the new ring frame and the command buffer the way psp_gfx_if.h describes them,
 *     checks them, and writes the fence. It cannot show what the real PSP accepts; it shows that
 *     the code under test drives the mailbox exactly as amdgpu did and builds well-formed frames.
 *
 * Checks:
 *   1. Every access to the PSP mailbox registers, reads and writes, equals the trace: same order,
 *      same register, same value. The ring is put at the MC address amdgpu used, so there is no
 *      exception list at all.
 *   2. Eleven commands, SETUP_TMR then ten LOAD_IP_FW in amdgpu's order and with amdgpu's types;
 *      sizes as the firmware headers give them; every image page aligned inside the staging area.
 *   3. Control runs that must fail: a PSP that never writes the fence (timeout, and nothing is
 *      submitted after it), and a PSP that reports a status (the sequence stops at that command).
 *   4. Ring stop issues GFX_CTRL_CMD_ID_DESTROY_RINGS.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc250_gmc.h"
#include "bc250_psp.h"

/* ---- unit A, E03 ------------------------------------------------------------------------------ */
#define UNITA_GART_TABLE_MC   0xF5FFE00000ull
#define UNITA_PSP_RING_MC     0xF5FFFE7000ull   /* C2PMSG_70:69 in the trace */
#define UNITA_TMR_MC          0xF5FF800000ull   /* kernel log: "reserve 0x400000 from 0xf5ff800000 for PSP TMR" */
/* Ours: where the miniport puts them (driver/kmd/psp.c keeps the same numbers). */
#define OUR_CMD_BUF_MC        0xF5FFFE8000ull
#define OUR_FENCE_MC          0xF5FFFE9000ull
#define OUR_STAGING_MC        0xF5FFC00000ull
#define OUR_STAGING_BYTES     0x200000u

#define BAR5_BYTES   0x80000u
#define BAR5_DWORDS  (BAR5_BYTES / 4u)
#define MBOX_FIRST   0x58200u   /* MP0_SMN_C2PMSG_64 */
#define MBOX_LAST    0x5821Cu   /* MP0_SMN_C2PMSG_71 */
#define REG_C2PMSG_64 0x58200u
#define REG_C2PMSG_67 0x5820Cu
#define REG_C2PMSG_69 0x58214u
#define REG_C2PMSG_70 0x58218u
#define REG_C2PMSG_71 0x5821Cu

/* ---- backend: registers ------------------------------------------------------------------------ */

struct access {
	char kind;      /* 'R' or 'W' */
	u32 off;
	u32 val;
};

static u32 g_value[BAR5_DWORDS];
static u32 g_sweep[BAR5_DWORDS];
static unsigned char g_known[BAR5_DWORDS];
static struct access g_log[4096];
static unsigned int g_log_count;
static unsigned int g_unknown_reads;
static int g_verbose;

static void psp_model_on_write(u32 off, u32 value);

static int load_sweep(const char *path)
{
	char line[512];
	int added = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		char name[256];
		unsigned int off = 0, val = 0;
		size_t len = strlen(line);

		if (len == 0 || line[len - 1] != '\n' || line[0] == '#')
			continue;
		if (sscanf(line, "%255s 0x%x %x", name, &off, &val) != 3)
			continue;
		if ((off & 3u) != 0 || off >= BAR5_BYTES || g_known[off / 4u])
			continue;
		g_sweep[off / 4u] = val;
		g_known[off / 4u] = 1;
		added++;
	}
	fclose(f);
	return added;
}

static void log_access(char kind, u32 off, u32 val)
{
	if (off < MBOX_FIRST || off > MBOX_LAST)
		return;
	if (g_log_count >= sizeof(g_log) / sizeof(g_log[0])) {
		fprintf(stderr, "access log full\n");
		exit(2);
	}
	g_log[g_log_count].kind = kind;
	g_log[g_log_count].off = off;
	g_log[g_log_count].val = val;
	g_log_count++;
}

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index)
{
	(void)adev;
	if (dword_index >= BAR5_DWORDS)
		exit(2);
	if (!g_known[dword_index])
		g_unknown_reads++;
	log_access('R', dword_index * 4u, g_value[dword_index]);
	return g_value[dword_index];
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value)
{
	(void)adev;
	if (dword_index >= BAR5_DWORDS)
		exit(2);
	g_value[dword_index] = value;
	g_known[dword_index] = 1;
	log_access('W', dword_index * 4u, value);
	psp_model_on_write(dword_index * 4u, value);
}

void bc250_shim_udelay(unsigned int usec)
{
	(void)usec;
}

void bc250_shim_log(int level, void *dev, const char *fmt, ...)
{
	va_list ap;

	(void)dev;
	if (!g_verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "[amdgpu %d] ", level);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/* ---- the PSP model ------------------------------------------------------------------------------ */

enum psp_mood { PSP_WELL, PSP_SILENT, PSP_REFUSES_FOURTH };

struct seen_cmd {
	u32 cmd_id;
	u32 fence;
	union psp_gfx_commands body;
};

static struct {
	enum psp_mood mood;
	/* the memory the model can see: MC address -> host pointer */
	u8 *ring;     u8 *cmd_buf;     u8 *fence;
	int ring_up;
	u32 rptr_dw;
	struct seen_cmd seen[32];
	unsigned int seen_count;
	unsigned int errors;
	unsigned int destroy_count;
} g_psp;

static void model_error(const char *what, u64 a, u64 b)
{
	printf("  PSP MODEL: %s (0x%llX, 0x%llX)\n", what, (unsigned long long)a, (unsigned long long)b);
	g_psp.errors++;
}

static void psp_model_on_write(u32 off, u32 value)
{
	if (off == REG_C2PMSG_64) {
		if (value == GFX_CTRL_CMD_ID_INIT_GPCOM_RING) {
			u64 ring_mc = ((u64)g_value[REG_C2PMSG_70 / 4] << 32) | g_value[REG_C2PMSG_69 / 4];

			if (ring_mc != UNITA_PSP_RING_MC || g_value[REG_C2PMSG_71 / 4] != 0x1000)
				model_error("ring create with an unexpected address or size", ring_mc, g_value[REG_C2PMSG_71 / 4]);
			g_psp.ring_up = 1;
			g_psp.rptr_dw = 0;
			g_value[REG_C2PMSG_67 / 4] = 0;
		} else if (value == GFX_CTRL_CMD_ID_DESTROY_RINGS) {
			g_psp.ring_up = 0;
			g_psp.destroy_count++;
		} else {
			model_error("unknown control command", value, 0);
		}
		/* Unit A's PSP: response flag set, the command id kept, status 0 (E03: 0x80020000). */
		g_value[REG_C2PMSG_64 / 4] = GFX_FLAG_RESPONSE | value;
		return;
	}
	if (off == REG_C2PMSG_67) {
		const struct psp_gfx_rb_frame *frame;
		const struct psp_gfx_cmd_resp *cmd = (const struct psp_gfx_cmd_resp *)g_psp.cmd_buf;
		struct psp_gfx_cmd_resp *cmd_w = (struct psp_gfx_cmd_resp *)g_psp.cmd_buf;
		struct seen_cmd *seen;
		u64 a;
		unsigned int i;

		if (!g_psp.ring_up)
			model_error("write pointer moved without a ring", value, 0);
		if (value != (g_psp.rptr_dw + 16u) % (0x1000u / 4u))
			model_error("write pointer did not advance by one frame", value, g_psp.rptr_dw);
		frame = (const struct psp_gfx_rb_frame *)(g_psp.ring + g_psp.rptr_dw * 4u);
		g_psp.rptr_dw = value;

		a = ((u64)frame->cmd_buf_addr_hi << 32) | frame->cmd_buf_addr_lo;
		if (a != OUR_CMD_BUF_MC)
			model_error("frame: command buffer address", a, OUR_CMD_BUF_MC);
		a = ((u64)frame->fence_addr_hi << 32) | frame->fence_addr_lo;
		if (a != OUR_FENCE_MC)
			model_error("frame: fence address", a, OUR_FENCE_MC);
		if (frame->cmd_buf_size != 0 || frame->sid_lo != 0 || frame->sid_hi != 0 || frame->vmid != 0 ||
		    frame->frame_type != 0)
			model_error("frame: a field amdgpu leaves 0 is not 0", frame->cmd_buf_size, frame->vmid);
		for (i = 0; i < 7; i++)
			if (frame->reserved2[i] != 0)
				model_error("frame: reserved word not 0", i, frame->reserved2[i]);
		if (cmd->resp.status != 0 || cmd->resp.fw_addr_lo != 0)
			model_error("command buffer: response area not cleared", cmd->resp.status, cmd->resp.fw_addr_lo);

		if (g_psp.seen_count < sizeof(g_psp.seen) / sizeof(g_psp.seen[0])) {
			seen = &g_psp.seen[g_psp.seen_count++];
			seen->cmd_id = cmd->cmd_id;
			seen->fence = frame->fence_value;
			seen->body = cmd->cmd;
		}

		if (g_psp.mood == PSP_SILENT)
			return;
		if (g_psp.mood == PSP_REFUSES_FOURTH && g_psp.seen_count == 4)
			cmd_w->resp.status = 0x0000000Au;
		if (cmd->cmd_id == GFX_CMD_ID_LOAD_IP_FW) {
			cmd_w->resp.fw_addr_lo = (u32)(UNITA_TMR_MC + 0x1000u * g_psp.seen_count);
			cmd_w->resp.fw_addr_hi = (u32)(UNITA_TMR_MC >> 32);
		}
		*(u32 *)g_psp.fence = frame->fence_value;
	}
}

/* ---- the trace ----------------------------------------------------------------------------------- */

static struct access g_trace[256];
static unsigned int g_trace_count;

static int load_trace(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		double t;
		char kind[8], name[256], rep[32];
		unsigned int off, val, times = 1, i;
		int fields = sscanf(line, "%lf %7s %255s 0x%x %x %31s", &t, kind, name, &off, &val, rep);

		if (fields < 5 || (kind[0] != 'R' && kind[0] != 'W'))
			continue;
		if (fields == 6 && rep[0] == 'x')
			times = (unsigned int)strtoul(rep + 1, NULL, 10);
		for (i = 0; i < times && g_trace_count < sizeof(g_trace) / sizeof(g_trace[0]); i++) {
			g_trace[g_trace_count].kind = kind[0];
			g_trace[g_trace_count].off = off;
			g_trace[g_trace_count].val = val;
			g_trace_count++;
		}
	}
	fclose(f);
	return (int)g_trace_count;
}

/* ---- firmware files ----------------------------------------------------------------------------- */

static u8 *g_file[BC250_FILE_COUNT];
static u32 g_file_size[BC250_FILE_COUNT];

static int load_firmware(const char *dir)
{
	unsigned int i;

	for (i = 0; i < BC250_FILE_COUNT; i++) {
		char path[1024];
		FILE *f;
		long n;

		snprintf(path, sizeof(path), "%s/%s", dir, bc250_fw_file_name((enum bc250_fw_file)i));
		f = fopen(path, "rb");
		if (f == NULL) {
			printf("cannot open %s\n", path);
			return -1;
		}
		fseek(f, 0, SEEK_END);
		n = ftell(f);
		fseek(f, 0, SEEK_SET);
		g_file[i] = (u8 *)malloc((size_t)n);
		if (g_file[i] == NULL || fread(g_file[i], 1, (size_t)n, f) != (size_t)n) {
			fclose(f);
			return -1;
		}
		g_file_size[i] = (u32)n;
		fclose(f);
	}
	return 0;
}

/* ---- one run -------------------------------------------------------------------------------------- */

struct run_result {
	int setup_rc, create_rc, tmr_rc, stop_rc;
	int fw_rc[BC250_FW_COUNT];
	unsigned int fw_done;
	u32 fw_size[BC250_FW_COUNT];
	u64 fw_mc[BC250_FW_COUNT];
	enum psp_gfx_fw_type fw_type[BC250_FW_COUNT];
	unsigned int staged_bytes;
};

static u8 g_ring[0x1000], g_cmd_buf[0x1000], g_fence[0x1000];
static u8 *g_staging;

static void run(enum psp_mood mood, struct run_result *res)
{
	struct amdgpu_device adev;
	struct amdgpu_bo gart_bo;
	struct bc250_gmc_inputs gmc_in;
	struct bc250_psp_inputs in;
	struct bc250_psp ctx;
	u32 staged = 0;
	unsigned int i;

	memset(res, 0, sizeof(*res));
	memset(&adev, 0, sizeof(adev));
	memset(&gart_bo, 0, sizeof(gart_bo));
	memset(&gmc_in, 0, sizeof(gmc_in));
	memset(&in, 0, sizeof(in));
	memset(&g_psp, 0, sizeof(g_psp));
	memset(g_ring, 0xA5, sizeof(g_ring));           /* stale bytes: the code must clear its frame itself */
	memset(g_cmd_buf, 0xA5, sizeof(g_cmd_buf));
	memset(g_fence, 0xA5, sizeof(g_fence));
	memcpy(g_value, g_sweep, sizeof(g_value));
	g_log_count = 0;
	g_unknown_reads = 0;
	g_psp.mood = mood;
	g_psp.ring = g_ring;
	g_psp.cmd_buf = g_cmd_buf;
	g_psp.fence = g_fence;
	adev.dev = (void *)"BC250-A";

	gmc_in.gart_table_mc = UNITA_GART_TABLE_MC;
	gmc_in.noretry = true;
	res->setup_rc = bc250_gmc_setup(&adev, &gmc_in, &gart_bo);       /* reads only: bases and VRAM window */
	if (res->setup_rc != 0)
		return;

	in.ring_mem = g_ring;       in.ring_mc = UNITA_PSP_RING_MC;
	in.cmd_buf = g_cmd_buf;     in.cmd_buf_mc = OUR_CMD_BUF_MC;
	in.fence_buf = g_fence;     in.fence_buf_mc = OUR_FENCE_MC;
	in.tmr_mc = UNITA_TMR_MC;
	res->setup_rc = bc250_psp_setup(&adev, &ctx, &in);
	if (res->setup_rc != 0)
		return;

	res->create_rc = bc250_psp_ring_create(&ctx);
	if (res->create_rc != 0)
		return;
	res->tmr_rc = bc250_psp_tmr_load(&ctx);
	if (res->tmr_rc == 0) {
		for (i = 0; i < BC250_FW_COUNT; i++) {
			enum bc250_fw_file file = bc250_fw_file_of((enum bc250_fw_id)i);
			u32 offset = 0, size = 0;

			res->fw_rc[i] = bc250_fw_locate((enum bc250_fw_id)i, g_file[file], g_file_size[file],
							&offset, &size, &res->fw_type[i]);
			if (res->fw_rc[i] != 0)
				break;
			if (staged + size > OUR_STAGING_BYTES) {
				res->fw_rc[i] = -1;
				break;
			}
			/* amdgpu_ucode_init_bo(): one image after the other, each on a page boundary */
			memcpy(g_staging + staged, g_file[file] + offset, size);
			res->fw_size[i] = size;
			res->fw_mc[i] = OUR_STAGING_MC + staged;
			staged += (size + 0xFFFu) & ~0xFFFu;
			res->fw_rc[i] = bc250_psp_load_ip_fw(&ctx, res->fw_type[i], res->fw_mc[i], size, NULL);
			if (res->fw_rc[i] != 0)
				break;
			res->fw_done++;
		}
	}
	res->staged_bytes = staged;
	res->stop_rc = bc250_psp_ring_stop(&ctx);
}

static const char *reg_name(u32 off)
{
	static char buf[32];

	snprintf(buf, sizeof(buf), "MP0_SMN_C2PMSG_%u", 64u + (off - MBOX_FIRST) / 4u);
	return buf;
}

int main(int argc, char **argv)
{
	static const u32 expect_type[BC250_FW_COUNT] = {
		GFX_FW_TYPE_SDMA0, GFX_FW_TYPE_SDMA1, GFX_FW_TYPE_CP_CE, GFX_FW_TYPE_CP_PFP, GFX_FW_TYPE_CP_ME,
		GFX_FW_TYPE_CP_MEC, GFX_FW_TYPE_CP_MEC_ME1, GFX_FW_TYPE_CP_MEC, GFX_FW_TYPE_CP_MEC_ME2, GFX_FW_TYPE_RLC_G,
	};
	struct run_result res;
	unsigned int i, bad = 0, compared, stop_accesses;
	int failures = 0;

	if (argc < 5) {
		printf("usage: replay_psp <sweep1> <sweep2> <trace-extract> <firmware dir> [-v]\n");
		return 2;
	}
	g_verbose = argc > 5 && strcmp(argv[5], "-v") == 0;
	{
		/* through volatile so that the comparison is not a constant expression (MSVC C4127) */
		volatile size_t layout[4] = { sizeof(struct psp_gfx_rb_frame), sizeof(struct psp_gfx_cmd_resp),
					      offsetof(struct psp_gfx_cmd_resp, resp), offsetof(struct psp_gfx_cmd_resp, cmd) };

		if (layout[0] != 64 || layout[1] != 1024 || layout[2] != 864 || layout[3] != 28) {
			printf("FAIL: structure layout differs from psp_gfx_if.h's own comments (frame %u, cmd %u, resp +%u, cmd +%u)\n",
			       (unsigned int)layout[0], (unsigned int)layout[1], (unsigned int)layout[2], (unsigned int)layout[3]);
			return 1;
		}
	}
	printf("layout: rb frame 64 bytes, command buffer 1024 bytes, response at +864: as psp_gfx_if.h says\n");
	if (load_sweep(argv[1]) < 0 || load_sweep(argv[2]) < 0 || load_trace(argv[3]) <= 0 || load_firmware(argv[4]) != 0) {
		printf("FAIL: cannot read the inputs\n");
		return 2;
	}
	g_staging = (u8 *)calloc(1, OUR_STAGING_BYTES);
	if (g_staging == NULL)
		return 2;

	/* ---- 1 and 2: the good run ------------------------------------------------------------------ */
	printf("\n== run with a PSP that answers\n");
	run(PSP_WELL, &res);
	printf("setup %d, ring create %d, tmr %d, images loaded %u of %u, ring stop %d, unknown reads %u\n",
	       res.setup_rc, res.create_rc, res.tmr_rc, res.fw_done, (unsigned int)BC250_FW_COUNT, res.stop_rc, g_unknown_reads);
	if (res.setup_rc || res.create_rc || res.tmr_rc || res.fw_done != BC250_FW_COUNT || res.stop_rc || g_unknown_reads)
		failures++;

	/* the ring stop at the end has no counterpart in the trace window: compare up to it */
	stop_accesses = 0;
	for (i = g_log_count; i > 0 && !(g_log[i - 1].kind == 'W' && g_log[i - 1].off == REG_C2PMSG_67); i--)
		stop_accesses++;
	compared = g_log_count - stop_accesses;
	printf("mailbox accesses: %u produced before the ring stop, %u in the trace\n", compared, g_trace_count);
	for (i = 0; i < compared || i < g_trace_count; i++) {
		const struct access *o = i < compared ? &g_log[i] : NULL, *t = i < g_trace_count ? &g_trace[i] : NULL;

		if (o == NULL || t == NULL || o->kind != t->kind || o->off != t->off || o->val != t->val) {
			bad++;
			if (bad <= 10)
				printf("  [%u] ours %c 0x%05X %08X   trace %c 0x%05X %08X\n", i,
				       o ? o->kind : '-', o ? o->off : 0, o ? o->val : 0,
				       t ? t->kind : '-', t ? t->off : 0, t ? t->val : 0);
		}
	}
	printf("register protocol: %s (%u differences)\n", bad == 0 ? "EXACT MATCH, reads included" : "MISMATCH", bad);
	if (bad != 0)
		failures++;
	printf("ring stop: %u accesses:", stop_accesses);
	for (i = compared; i < g_log_count; i++)
		printf(" %c %s %08X", g_log[i].kind, reg_name(g_log[i].off), g_log[i].val);
	printf("\n");
	if (g_psp.destroy_count != 1)
		failures++;

	printf("commands the PSP model received: %u, model complaints: %u\n", g_psp.seen_count, g_psp.errors);
	if (g_psp.seen_count != 1 + BC250_FW_COUNT || g_psp.errors != 0)
		failures++;
	for (i = 0; i < g_psp.seen_count; i++) {
		const struct seen_cmd *s = &g_psp.seen[i];

		if (s->fence != i + 1) {
			printf("  FAIL fence value %u at command %u\n", s->fence, i);
			failures++;
		}
		if (i == 0) {
			const struct psp_gfx_cmd_setup_tmr *c = &s->body.cmd_setup_tmr;
			u64 mc = ((u64)c->buf_phy_addr_hi << 32) | c->buf_phy_addr_lo;
			u64 pa = ((u64)c->system_phy_addr_hi << 32) | c->system_phy_addr_lo;

			printf("  [ 1] cmd 0x%02X SETUP_TMR  MC 0x%llX size 0x%X flags 0x%X physical 0x%llX\n", s->cmd_id,
			       (unsigned long long)mc, c->buf_size, c->tmr_flags, (unsigned long long)pa);
			if (s->cmd_id != GFX_CMD_ID_SETUP_TMR || mc != UNITA_TMR_MC || c->buf_size != 0x400000 ||
			    c->tmr_flags != 2 || pa != 0x46F800000ull) {
				printf("  FAIL SETUP_TMR contents\n");
				failures++;
			}
		} else {
			const struct psp_gfx_cmd_load_ip_fw *c = &s->body.cmd_load_ip_fw;
			u64 mc = ((u64)c->fw_phy_addr_hi << 32) | c->fw_phy_addr_lo;
			unsigned int id = i - 1;

			printf("  [%2u] cmd 0x%02X LOAD_IP_FW %-10s type %2u  MC 0x%llX  %6u bytes\n", i + 1, s->cmd_id,
			       bc250_fw_name((enum bc250_fw_id)id), (unsigned int)c->fw_type, (unsigned long long)mc, c->fw_size);
			if (s->cmd_id != GFX_CMD_ID_LOAD_IP_FW || (u32)c->fw_type != expect_type[id] || mc != res.fw_mc[id] ||
			    c->fw_size != res.fw_size[id] || (mc & 0xFFF) != 0 || mc < OUR_STAGING_MC ||
			    mc + c->fw_size > OUR_STAGING_MC + OUR_STAGING_BYTES) {
				printf("  FAIL LOAD_IP_FW contents\n");
				failures++;
			}
		}
	}
	printf("staging area used: 0x%X of 0x%X bytes\n", res.staged_bytes, OUR_STAGING_BYTES);
	/* mec and mec2 split: image + jump table must add up to the file's ucode size */
	if (res.fw_size[BC250_FW_CP_MEC1] + res.fw_size[BC250_FW_CP_MEC1_JT] !=
	    ((const struct common_firmware_header *)g_file[BC250_FILE_MEC])->ucode_size_bytes) {
		printf("FAIL: MEC image and jump table do not add up\n");
		failures++;
	}

	/* ---- 3: control runs that must fail ---------------------------------------------------------- */
	printf("\n== control: a PSP that never writes the fence\n");
	run(PSP_SILENT, &res);
	printf("tmr %d, images loaded %u, commands the model saw %u\n", res.tmr_rc, res.fw_done, g_psp.seen_count);
	if (res.tmr_rc == 0 || res.fw_done != 0 || g_psp.seen_count != 1) {
		printf("FAIL: the timeout was not noticed or the sequence went on\n");
		failures++;
	}
	printf("\n== control: a PSP that refuses the fourth command\n");
	run(PSP_REFUSES_FOURTH, &res);
	printf("tmr %d, images loaded %u, commands the model saw %u, rc of the refused one %d\n",
	       res.tmr_rc, res.fw_done, g_psp.seen_count, res.fw_rc[2]);
	if (res.tmr_rc != 0 || res.fw_done != 2 || g_psp.seen_count != 4 || res.fw_rc[2] == 0) {
		printf("FAIL: the refused command did not stop the sequence\n");
		failures++;
	}

	printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
