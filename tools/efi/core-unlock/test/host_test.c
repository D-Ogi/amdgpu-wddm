/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host tests of core-unlock: everything but src/main.c, with the firmware replaced by models. The SMU tests run the
 * shim's real transport and policy (driver/shim/bc250_smu.c, bc250_cpu.c) against a model of queue 3; the board
 * tests run the driver's real BoardIdentitySelect over an SMBIOS table built here.
 *
 * The numbers in the synthetic ACPI and SMBIOS tables below are test data, not facts about the board. The only
 * register numbers come from the shim, by name.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "cu_acpi.h"
#include "cu_decide.h"
#include "cu_run.h"
#include "cu_smbios.h"
#include "cu_smu.h"
#include "cu_text.h"
#include "bc250_cpu.h"
#include "board_identity.h"
#include "generated/smu_registers.h"
#include "thm_10_0_sh_mask.h"

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- the decision table -------------------------------------------------------------------------- */

static void test_decide(void)
{
	static const unsigned counts[] = { 0u, 8u, 12u, 14u, 16u, 32u };
	unsigned b, c, m, k;
	struct cu_inputs in;
	struct cu_decision d;

	/* Every combination: the invariants of the loop guard. */
	for (b = 0; b < 2; b++)
		for (c = 0; c < sizeof(counts) / sizeof(counts[0]); c++)
			for (m = CU_MARKER_NONE; m <= CU_MARKER_ERROR; m++)
				for (k = 0; k < 2; k++) {
					in.board_ok = (int)b;
					in.processors = counts[c];
					in.marker = (enum cu_marker)m;
					in.key_pressed = (int)k;
					d = cu_decide(&in);
					CHECK(d.reason < CU_WHY_COUNT);
					/* The unlock needs all four conditions, and nothing else gives it. */
					CHECK((d.action == CU_ACT_UNLOCK) ==
					      (b && counts[c] == 12u && m == CU_MARKER_NONE && !k));
					/* A foreign board, or a count other than 12 or 16, changes nothing at all. */
					if (!b || (counts[c] != 12u && counts[c] != 16u)) CHECK(d.action == CU_ACT_BOOT);
					/* A failed mark follows an attempt only, never a second reset. */
					CHECK((d.action == CU_ACT_MARK_FAILED) ==
					      (b && counts[c] == 12u && m == CU_MARKER_ATTEMPT));
					CHECK((d.action == CU_ACT_CLEAR) ==
					      (b && counts[c] == 16u && m != CU_MARKER_NONE && m != CU_MARKER_ERROR));
				}
	/* The rows by reason. */
	in.board_ok = 0; in.processors = 12; in.marker = CU_MARKER_NONE; in.key_pressed = 0;
	CHECK(cu_decide(&in).reason == CU_WHY_NOT_BOARD);
	in.board_ok = 1;
	CHECK(cu_decide(&in).reason == CU_WHY_STOCK);
	in.key_pressed = 1;
	CHECK(cu_decide(&in).reason == CU_WHY_KEY);
	in.key_pressed = 0; in.marker = CU_MARKER_ATTEMPT;
	CHECK(cu_decide(&in).reason == CU_WHY_ATTEMPT_FAILED);
	in.marker = CU_MARKER_FAILED;
	CHECK(cu_decide(&in).reason == CU_WHY_EARLIER_FAILED);
	in.marker = CU_MARKER_UNKNOWN;
	CHECK(cu_decide(&in).reason == CU_WHY_EARLIER_FAILED);
	in.marker = CU_MARKER_ERROR;
	CHECK(cu_decide(&in).reason == CU_WHY_MARKER_ERROR);
	in.processors = 16; in.marker = CU_MARKER_NONE;
	CHECK(cu_decide(&in).reason == CU_WHY_FULL);
	in.marker = CU_MARKER_FAILED;
	CHECK(cu_decide(&in).reason == CU_WHY_FULL_AFTER);
	in.processors = 0;
	CHECK(cu_decide(&in).reason == CU_WHY_COUNT_UNKNOWN);
}

static void test_marker_text(void)
{
	const char *a = cu_marker_text(CU_MARKER_ATTEMPT), *f = cu_marker_text(CU_MARKER_FAILED);
	CHECK(a != NULL && f != NULL && strcmp(a, f) != 0);
	CHECK(cu_marker_parse((const unsigned char *)a, (unsigned long)strlen(a)) == CU_MARKER_ATTEMPT);
	CHECK(cu_marker_parse((const unsigned char *)f, (unsigned long)strlen(f)) == CU_MARKER_FAILED);
	CHECK(cu_marker_parse((const unsigned char *)a, (unsigned long)strlen(a) - 1u) == CU_MARKER_UNKNOWN);
	CHECK(cu_marker_parse((const unsigned char *)"", 0) == CU_MARKER_UNKNOWN);
	CHECK(cu_marker_parse(NULL, 0) == CU_MARKER_UNKNOWN);
	CHECK(cu_marker_text(CU_MARKER_NONE) == NULL);
}

/* ---- the SMU message, against a model of queue 3 -------------------------------------------------- */

#define MODEL_LOG 64
struct model {
	struct bc250_cpu_queue q;
	unsigned resp, param;		/* the two registers the firmware answers in */
	unsigned temperature_raw;
	unsigned answer_mv;		/* what the voltage getter answers */
	unsigned getter_response, store_response;	/* the response word of each message, 0 for none */
	unsigned messages, message[8], argument[8];
	unsigned writes, write_offset[MODEL_LOG], write_value[MODEL_LOG];
	unsigned long long stalled_us;
	unsigned long long stalled_before_store;
	int foreign_access;		/* an access to an offset that is neither queue 3 nor the temperature */
};

static unsigned raw_temperature(int celsius)
{
	return ((unsigned)(celsius * 1000 / 125) << THM_TCON_CUR_TMP__CUR_TEMP__SHIFT) &
	       (unsigned)THM_TCON_CUR_TMP__CUR_TEMP_MASK;
}

static int model_read(void *context, unsigned offset, unsigned *value)
{
	struct model *m = context;
	if (offset == m->q.resp_reg) *value = m->resp;
	else if (offset == m->q.param_reg) *value = m->param;
	else if (offset == BC250_SMU_mmTHM_TCON_CUR_TMP) *value = m->temperature_raw;
	else { m->foreign_access = 1; *value = 0xFFFFFFFFu; }
	return 0;
}

static int model_write(void *context, unsigned offset, unsigned value)
{
	struct model *m = context;
	if (m->writes < MODEL_LOG) { m->write_offset[m->writes] = offset; m->write_value[m->writes] = value; }
	m->writes++;
	if (offset == m->q.resp_reg) m->resp = value;
	else if (offset == m->q.param_reg) m->param = value;
	else if (offset == m->q.msg_reg) {
		if (m->messages < 8) { m->message[m->messages] = value; m->argument[m->messages] = m->param; }
		m->messages++;
		if (value == BC250_CPU_MSG_READ_CPU_MV) {
			m->resp = m->getter_response;
			if (m->getter_response == 1u) m->param = m->answer_mv;
		} else {
			m->resp = m->store_response;
			m->stalled_before_store = m->stalled_us;
		}
	} else m->foreign_access = 1;
	return 0;
}

static void model_stall(void *context, unsigned usec)
{
	((struct model *)context)->stalled_us += usec;
}

static void model_init(struct model *m, struct cu_regs *regs)
{
	memset(m, 0, sizeof(*m));
	bc250_cpu_queue3(&m->q);
	m->resp = 1u;				/* the BIOS left the last response at OK */
	m->temperature_raw = raw_temperature(45);
	m->answer_mv = 1100u;
	m->getter_response = 1u;
	m->store_response = 1u;
	regs->context = m;
	regs->read32 = model_read;
	regs->write32 = model_write;
	regs->stall_us = model_stall;
}

static void test_smu_sent(void)
{
	struct model m;
	struct cu_regs regs;
	struct cu_unlock_report r;
	unsigned i;
	model_init(&m, &regs);
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_SENT);
	CHECK(r.result == CU_UNLOCK_SENT && r.status == 0 && r.response == 1u && r.millivolts == 1100u);
	CHECK(r.temperature_valid && r.temperature_mc == 45000);
	/* Two messages: the getter, then the store with the one admitted argument. */
	CHECK(m.messages == 2u && r.messages == 2u);
	CHECK(m.message[0] == BC250_CPU_MSG_READ_CPU_MV && m.argument[0] == 0u);
	CHECK(m.message[1] == BC250_CPU_MSG_STORE_FF && m.argument[1] == BC250_CPU_SMN_CORE_ENABLE_MASK);
	CHECK(m.argument[1] != 0u);
	/* The AMD send order for each message: response cleared, argument, command. */
	CHECK(m.writes == 6u);
	for (i = 0; i < 2u; i++) {
		CHECK(m.write_offset[3u * i] == m.q.resp_reg && m.write_value[3u * i] == 0u);
		CHECK(m.write_offset[3u * i + 1u] == m.q.param_reg);
		CHECK(m.write_offset[3u * i + 2u] == m.q.msg_reg);
	}
	/* The slow gap of a start's first queue 3 traffic, before the store. */
	CHECK(m.stalled_before_store >= BC250_CPU_MESSAGE_GAP_MS * 1000ull);
	CHECK(!m.foreign_access);
}

static void test_smu_failures(void)
{
	struct model m;
	struct cu_regs regs;
	struct cu_unlock_report r;

	/* The firmware is still busy with an earlier command: nothing is written at all. */
	model_init(&m, &regs);
	m.resp = 0u;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_READ_STAGE);
	CHECK(m.messages == 0u && m.writes == 0u);
	/* The poll is bounded: the stall clock reached the transport's 20 ms and stopped there. */
	CHECK(m.stalled_us >= 20000u && m.stalled_us < 1000000u);

	/* The getter is refused: the store never goes. */
	model_init(&m, &regs);
	m.getter_response = 0xFEu;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_READ_STAGE);
	CHECK(m.messages == 1u && r.response == 0xFEu);

	/* The getter never answers: the store never goes. */
	model_init(&m, &regs);
	m.getter_response = 0u;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_READ_STAGE);
	CHECK(m.messages == 1u);

	/* A voltage outside the plausible band: the queue answered something we do not understand. */
	model_init(&m, &regs);
	m.answer_mv = 0u;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_VOLTAGE);
	CHECK(m.messages == 1u);
	model_init(&m, &regs);
	m.answer_mv = BC250_CPU_PLAUSIBLE_MAX_MV + 1u;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_VOLTAGE);
	CHECK(m.messages == 1u);

	/* Hot: the getter still goes, the store does not. */
	model_init(&m, &regs);
	m.temperature_raw = raw_temperature(BC250_CLOCK_HOT_MC / 1000);
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_HOT);
	CHECK(m.messages == 1u && m.message[0] == BC250_CPU_MSG_READ_CPU_MV);
	CHECK(r.temperature_valid && r.temperature_mc == BC250_CLOCK_HOT_MC);

	/* One step under the hot limit: sent. */
	model_init(&m, &regs);
	m.temperature_raw = raw_temperature(BC250_CLOCK_HOT_MC / 1000 - 1);
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_SENT);

	/* No temperature: the setter is not sent (the KMD's AllowHot FALSE). */
	model_init(&m, &regs);
	m.temperature_raw = 0xFFFFFFFFu;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_HOT);
	CHECK(m.messages == 1u && !r.temperature_valid);

	/* The store is refused or not answered. */
	model_init(&m, &regs);
	m.store_response = 0xFFu;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_REFUSED);
	CHECK(m.messages == 2u && r.response == 0xFFu && r.status != 0);
	model_init(&m, &regs);
	m.store_response = 0u;
	CHECK(cu_unlock_send(&regs, &r) == CU_UNLOCK_REFUSED);
	CHECK(m.messages == 2u);

	/* The policy that guards the store: argument 0 hangs the firmware, and only the named register passes. */
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_STORE_FF, 0u));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_STORE_FF, BC250_CPU_SMN_CORE_ENABLE_MASK));
	CHECK(cu_unlock_result_text(CU_UNLOCK_COUNT) != NULL);
}

/* ---- the steps of one start, against recorded operations ----------------------------------------- */

struct script {
	int board;
	unsigned processors;
	enum cu_marker marker;
	int key;
	int write_fails, delete_fails, unlock_fails;
	char trace[128];
	size_t used;
	unsigned lines;
};

static void trace(struct script *s, const char *step)
{
	while (*step && s->used + 1u < sizeof(s->trace)) s->trace[s->used++] = *step++;
	s->trace[s->used] = 0;
}

static int s_board(void *c) { trace(c, "B"); return ((struct script *)c)->board; }
static unsigned s_processors(void *c) { trace(c, "P"); return ((struct script *)c)->processors; }
static enum cu_marker s_marker_read(void *c) { trace(c, "M"); return ((struct script *)c)->marker; }
static int s_marker_write(void *c, enum cu_marker state)
{
	struct script *s = c;
	trace(s, state == CU_MARKER_ATTEMPT ? "Wa" : state == CU_MARKER_FAILED ? "Wf" : "W?");
	if (s->write_fails) return -1;
	s->marker = state;
	return 0;
}
static int s_marker_delete(void *c)
{
	struct script *s = c;
	trace(s, "D");
	if (s->delete_fails) return -1;
	s->marker = CU_MARKER_NONE;
	return 0;
}
static int s_key(void *c) { trace(c, "K"); return ((struct script *)c)->key; }
static int s_unlock(void *c) { trace(c, "U"); return ((struct script *)c)->unlock_fails ? -1 : 0; }
static int s_reset(void *c) { trace(c, "R"); return -1; }
static void s_say(void *c, const char *line) { (void)line; ((struct script *)c)->lines++; }

static enum cu_outcome run_script(struct script *s, int resets)
{
	struct cu_ops ops;
	ops.context = s;
	ops.board = s_board;
	ops.processors = s_processors;
	ops.marker_read = s_marker_read;
	ops.marker_write = s_marker_write;
	ops.marker_delete = s_marker_delete;
	ops.key_window = s_key;
	ops.unlock = s_unlock;
	ops.reset = s_reset;
	ops.say = s_say;
	s->used = 0;
	s->trace[0] = 0;
	return cu_run(&ops, resets);
}

static void script_init(struct script *s, int board, unsigned processors, enum cu_marker marker)
{
	memset(s, 0, sizeof(*s));
	s->board = board;
	s->processors = processors;
	s->marker = marker;
}

static void test_run(void)
{
	struct script s;

	script_init(&s, 0, 12u, CU_MARKER_NONE);
	CHECK(run_script(&s, 1) == CU_OUT_NOTHING);
	CHECK(!strcmp(s.trace, "B"));		/* a foreign board: no other question is asked */

	/* The unlock: the window, the marker, the message, the reset. The model's reset returns, which is the
	 * "reset did not happen" path, so the marker goes again. */
	script_init(&s, 1, 12u, CU_MARKER_NONE);
	CHECK(run_script(&s, 1) == CU_OUT_FAILED);
	CHECK(!strcmp(s.trace, "BPMKWaURD"));

	script_init(&s, 1, 12u, CU_MARKER_NONE);
	s.key = 1;
	CHECK(run_script(&s, 1) == CU_OUT_NOTHING);
	CHECK(!strcmp(s.trace, "BPMK"));

	/* No guard on the disk: no message and no reset. */
	script_init(&s, 1, 12u, CU_MARKER_NONE);
	s.write_fails = 1;
	CHECK(run_script(&s, 1) == CU_OUT_FAILED);
	CHECK(!strcmp(s.trace, "BPMKWa"));

	/* The message failed: the marker goes, no reset. */
	script_init(&s, 1, 12u, CU_MARKER_NONE);
	s.unlock_fails = 1;
	CHECK(run_script(&s, 1) == CU_OUT_FAILED);
	CHECK(!strcmp(s.trace, "BPMKWaUD"));
	CHECK(s.marker == CU_MARKER_NONE);

	/* The start after an attempt that did not give the cores: marked failed, no message, no reset. */
	script_init(&s, 1, 12u, CU_MARKER_ATTEMPT);
	CHECK(run_script(&s, 1) == CU_OUT_MARKED_FAILED);
	CHECK(!strcmp(s.trace, "BPMDWf"));
	CHECK(s.marker == CU_MARKER_FAILED);
	/* And the start after that one does nothing at all. */
	CHECK(run_script(&s, 1) == CU_OUT_NOTHING);
	CHECK(!strcmp(s.trace, "BPM"));

	/* A delete that fails keeps the attempt marker: the next start marks it failed again, and never resets. */
	script_init(&s, 1, 12u, CU_MARKER_ATTEMPT);
	s.delete_fails = 1;
	CHECK(run_script(&s, 1) == CU_OUT_MARKED_FAILED);
	CHECK(!strcmp(s.trace, "BPMD"));
	CHECK(run_script(&s, 1) == CU_OUT_MARKED_FAILED);
	CHECK(strchr(s.trace, 'U') == NULL && strchr(s.trace, 'R') == NULL);

	/* The start after an attempt that worked. */
	script_init(&s, 1, 16u, CU_MARKER_ATTEMPT);
	CHECK(run_script(&s, 1) == CU_OUT_CLEARED);
	CHECK(!strcmp(s.trace, "BPMD"));
	script_init(&s, 1, 16u, CU_MARKER_NONE);
	CHECK(run_script(&s, 1) == CU_OUT_NOTHING);
	CHECK(!strcmp(s.trace, "BPM"));

	/* reset=none: no marker, the message, no reset. */
	script_init(&s, 1, 12u, CU_MARKER_NONE);
	CHECK(run_script(&s, 0) == CU_OUT_SENT);
	CHECK(!strcmp(s.trace, "BPMKU"));
	script_init(&s, 1, 12u, CU_MARKER_NONE);
	s.unlock_fails = 1;
	CHECK(run_script(&s, 0) == CU_OUT_FAILED);
	CHECK(!strcmp(s.trace, "BPMKU"));

	/* An unreadable marker and an unknown count change nothing. */
	script_init(&s, 1, 12u, CU_MARKER_ERROR);
	CHECK(run_script(&s, 1) == CU_OUT_NOTHING);
	CHECK(!strcmp(s.trace, "BPM"));
	script_init(&s, 1, 0u, CU_MARKER_NONE);
	CHECK(run_script(&s, 1) == CU_OUT_NOTHING);
	CHECK(!strcmp(s.trace, "BPM"));
	CHECK(s.lines >= 1u);
}

/* ---- ACPI ---------------------------------------------------------------------------------------- */

#define MEM_BASE 0x10000ull
#define MEM_BYTES 0x8000u
static unsigned char mem[MEM_BYTES];

static const unsigned char *mem_map(void *context, unsigned long long address, unsigned long bytes)
{
	(void)context;
	if (address < MEM_BASE || address + bytes > MEM_BASE + MEM_BYTES) return NULL;
	return mem + (address - MEM_BASE);
}

static void put32(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void put64(unsigned char *p, unsigned long long v) { put32(p, (unsigned)v); put32(p + 4, (unsigned)(v >> 32)); }

static void fix_checksum(unsigned char *p, unsigned bytes, unsigned at)
{
	unsigned char sum = 0;
	unsigned i;
	p[at] = 0;
	for (i = 0; i < bytes; i++) sum = (unsigned char)(sum + p[i]);
	p[at] = (unsigned char)(0u - sum);
}

static void sdt(unsigned long long address, const char *signature, unsigned bytes)
{
	unsigned char *t = mem + (address - MEM_BASE);
	memcpy(t, signature, 4);
	put32(t + 4, bytes);
	t[8] = 6;
}

#define RSDP_AT 0x10000ull
#define XSDT_AT 0x10100ull
#define RSDT_AT 0x10200ull
#define FADT_AT 0x11000ull
#define MADT_AT 0x12000ull
#define FADT_BYTES 276u

static void build_acpi(int revision, unsigned madt_local_enabled, unsigned madt_local_disabled, unsigned madt_x2)
{
	unsigned char *r = mem + (RSDP_AT - MEM_BASE), *f, *m;
	unsigned at = 44, i, madt_bytes;
	memset(mem, 0, sizeof(mem));
	memcpy(r, "RSD PTR ", 8);
	r[15] = (unsigned char)revision;
	put32(r + 16, (unsigned)RSDT_AT);
	put32(r + 20, 36);
	put64(r + 24, revision >= 2 ? XSDT_AT : 0);
	fix_checksum(r, 20, 8);
	fix_checksum(r, 36, 32);
	sdt(XSDT_AT, "XSDT", 36 + 16);
	put64(mem + (XSDT_AT - MEM_BASE) + 36, FADT_AT);
	put64(mem + (XSDT_AT - MEM_BASE) + 44, MADT_AT);
	fix_checksum(mem + (XSDT_AT - MEM_BASE), 52, 9);
	sdt(RSDT_AT, "RSDT", 36 + 8);
	put32(mem + (RSDT_AT - MEM_BASE) + 36, (unsigned)FADT_AT);
	put32(mem + (RSDT_AT - MEM_BASE) + 40, (unsigned)MADT_AT);
	fix_checksum(mem + (RSDT_AT - MEM_BASE), 44, 9);
	/* A FADT with a reset register in I/O space. Test data: port 0x0ABC, value 0x5A. */
	sdt(FADT_AT, "FACP", FADT_BYTES);
	f = mem + (FADT_AT - MEM_BASE);
	put32(f + 112, 1u << 10);
	f[116] = 1; f[117] = 8; f[118] = 0; f[119] = 1;
	put64(f + 120, 0x0ABCu);
	f[128] = 0x5A;
	fix_checksum(f, FADT_BYTES, 9);
	/* A MADT. */
	m = mem + (MADT_AT - MEM_BASE);
	for (i = 0; i < madt_local_enabled + madt_local_disabled; i++, at += 8) {
		m[at] = 0; m[at + 1] = 8; m[at + 2] = (unsigned char)i; m[at + 3] = (unsigned char)i;
		put32(m + at + 4, i < madt_local_enabled ? 1u : 0u);
	}
	for (i = 0; i < madt_x2; i++, at += 16) {
		m[at] = 9; m[at + 1] = 16;
		put32(m + at + 4, 0x100u + i);
		put32(m + at + 8, 1u);
	}
	m[at] = 1; m[at + 1] = 12;	/* an I/O APIC entry, which is not a processor */
	at += 12;
	madt_bytes = at;
	sdt(MADT_AT, "APIC", madt_bytes);
	fix_checksum(m, madt_bytes, 9);
}

static void test_acpi(void)
{
	struct cu_acpi a;
	struct cu_reset_register reg;
	unsigned n = 0;
	a.map = mem_map;
	a.context = NULL;
	a.rsdp = RSDP_AT;

	build_acpi(2, 12, 4, 0);
	CHECK(cu_acpi_reset_register(&a, &reg) == CU_ACPI_OK);
	CHECK(reg.space == CU_ACPI_SPACE_IO && reg.address == 0x0ABCu && reg.value == 0x5Au);
	CHECK(cu_acpi_madt_processors(&a, &n) == CU_ACPI_OK && n == 12u);

	/* Revision 0: the RSDT. */
	build_acpi(0, 16, 0, 0);
	CHECK(cu_acpi_reset_register(&a, &reg) == CU_ACPI_OK && reg.value == 0x5Au);
	CHECK(cu_acpi_madt_processors(&a, &n) == CU_ACPI_OK && n == 16u);

	/* x2APIC entries count only when there is no Local APIC entry. */
	build_acpi(2, 0, 0, 16);
	CHECK(cu_acpi_madt_processors(&a, &n) == CU_ACPI_OK && n == 16u);
	build_acpi(2, 12, 0, 16);
	CHECK(cu_acpi_madt_processors(&a, &n) == CU_ACPI_OK && n == 12u);

	/* The flag RESET_REG_SUP clear. */
	build_acpi(2, 12, 0, 0);
	put32(mem + (FADT_AT - MEM_BASE) + 112, 0);
	fix_checksum(mem + (FADT_AT - MEM_BASE), FADT_BYTES, 9);
	CHECK(cu_acpi_reset_register(&a, &reg) == CU_ACPI_NOT_SUPPORTED);

	/* A register that is not 8 bits wide. */
	build_acpi(2, 12, 0, 0);
	mem[FADT_AT - MEM_BASE + 117] = 16;
	fix_checksum(mem + (FADT_AT - MEM_BASE), FADT_BYTES, 9);
	CHECK(cu_acpi_reset_register(&a, &reg) == CU_ACPI_BAD_REGISTER);

	/* A wrong checksum hides the table. */
	build_acpi(2, 12, 0, 0);
	mem[FADT_AT - MEM_BASE + 128] ^= 1u;
	CHECK(cu_acpi_reset_register(&a, &reg) == CU_ACPI_NO_TABLE);

	/* A FADT that ends before the reset value. */
	build_acpi(2, 12, 0, 0);
	put32(mem + (FADT_AT - MEM_BASE) + 4, 116);
	fix_checksum(mem + (FADT_AT - MEM_BASE), 116, 9);
	CHECK(cu_acpi_reset_register(&a, &reg) == CU_ACPI_SHORT);

	/* No RSDP. */
	build_acpi(2, 12, 0, 0);
	mem[0] = 'X';
	CHECK(cu_acpi_reset_register(&a, &reg) == CU_ACPI_NO_RSDP);
	a.rsdp = 0;
	CHECK(cu_acpi_madt_processors(&a, &n) == CU_ACPI_NO_RSDP && n == 0u);
}

/* ---- SMBIOS and the driver's board identity ------------------------------------------------------ */

static size_t add_structure(unsigned char *t, size_t at, const unsigned char *formatted, size_t bytes,
			    const char *const *strings, unsigned count)
{
	unsigned i;
	memcpy(t + at, formatted, bytes);
	at += bytes;
	for (i = 0; i < count; i++) {
		size_t n = strlen(strings[i]) + 1u;
		memcpy(t + at, strings[i], n);
		at += n;
	}
	if (count == 0) t[at++] = 0;
	t[at++] = 0;
	return at;
}

/* The identity strings are the ones driver/kmd/board_identity.c accepts. */
static size_t build_smbios(unsigned char *t, const char *product)
{
	static const char *const bios[] = { "American Megatrends Inc.", "P5.00", "05/03/2022" };
	const char *board[] = { "ASRock", product };
	unsigned char b0[0x12] = { 0, 0x12, 0, 0, 1, 2, 0, 0, 3 };
	unsigned char b1[0x1B] = { 1, 0x1B, 1, 0 };
	unsigned char b2[0x08] = { 2, 0x08, 2, 0, 1, 2 };
	unsigned char end[4] = { 127, 4, 3, 0 };
	size_t at = 0;
	unsigned i;
	for (i = 0; i < 16; i++) b1[8 + i] = (unsigned char)(0x10 + i);
	at = add_structure(t, at, b0, sizeof(b0), bios, 3);
	at = add_structure(t, at, b1, sizeof(b1), NULL, 0);
	at = add_structure(t, at, b2, sizeof(b2), board, 2);
	at = add_structure(t, at, end, sizeof(end), NULL, 0);
	return at;
}

static void build_pci(unsigned char *pci)
{
	memset(pci, 0, 64);
	pci[0] = 0x02; pci[1] = 0x10;	/* vendor 0x1002 */
	pci[2] = 0xFE; pci[3] = 0x13;	/* device 0x13FE */
	pci[44] = 0x22; pci[45] = 0x10;	/* subsystem vendor 0x1022, subsystem 0 */
}

static void test_smbios(void)
{
	unsigned char table[512], rsmb[600], pci[64], tail[600];
	size_t n, extent, bytes;
	unsigned reason = 0, bios = 0;
	unsigned char machine[16];
	struct cu_smbios_table found;
	unsigned char *e;

	n = build_smbios(table, "AMD BC-250");
	/* Bytes after the end-of-table structure (a 3.0 maximum is a bound, not a length) are not part of it. */
	memcpy(tail, table, n);
	memset(tail + n, 0xAA, 32);
	extent = cu_smbios_extent(tail, n + 32u);
	CHECK(extent == n);
	bytes = cu_smbios_rsmb(tail, extent, 3, 4, rsmb, sizeof(rsmb));
	CHECK(bytes == n + CU_RSMB_HEADER_BYTES);
	build_pci(pci);
	CHECK(BoardIdentitySelect(pci, 64, rsmb, bytes, &reason) == 1u);
	CHECK(BoardIdentityQualification(rsmb, bytes, &bios, machine) == 1 && bios == 5u);
	/* The PCI function alone answers reason 3, as main.c reads it. */
	CHECK(BoardIdentitySelect(pci, 64, NULL, 0, &reason) == 0u && reason == 3u);
	/* Another product, or another PCI function, is not a BC-250. */
	n = build_smbios(table, "AMD BC-251");
	bytes = cu_smbios_rsmb(table, cu_smbios_extent(table, n), 3, 4, rsmb, sizeof(rsmb));
	CHECK(BoardIdentitySelect(pci, 64, rsmb, bytes, &reason) == 0u);
	n = build_smbios(table, "AMD BC-250");
	bytes = cu_smbios_rsmb(table, cu_smbios_extent(table, n), 3, 4, rsmb, sizeof(rsmb));
	pci[2] = 0xFF;
	CHECK(BoardIdentitySelect(pci, 64, rsmb, bytes, &reason) == 0u && reason == 2u);
	/* A table cut before its end-of-table structure has no extent, and a small buffer gets nothing. */
	CHECK(cu_smbios_extent(table, n - 1u) == 0u);
	CHECK(cu_smbios_extent(table, 3) == 0u);
	CHECK(cu_smbios_rsmb(table, n, 3, 4, rsmb, n) == 0u);

	/* The two entry points, in the test memory. */
	memset(mem, 0, sizeof(mem));
	e = mem;
	memcpy(e, "_SM3_", 5);
	e[6] = 0x18; e[7] = 3; e[8] = 4;
	put32(e + 0x0C, 0x400u);
	put64(e + 0x10, MEM_BASE + 0x1000u);
	fix_checksum(e, 0x18, 5);
	CHECK(cu_smbios_entry(mem_map, NULL, MEM_BASE, 1, &found) == 1);
	CHECK(found.address == MEM_BASE + 0x1000u && found.max_bytes == 0x400u && found.major == 3u && found.minor == 4u);
	e[0x10] ^= 1u;
	CHECK(cu_smbios_entry(mem_map, NULL, MEM_BASE, 1, &found) == 0);
	e = mem + 0x100;
	memcpy(e, "_SM_", 4);
	e[5] = 0x1F; e[6] = 2; e[7] = 8;
	memcpy(e + 0x10, "_DMI_", 5);
	e[0x16] = 0x34; e[0x17] = 0x01;
	put32(e + 0x18, (unsigned)(MEM_BASE + 0x2000u));
	fix_checksum(e, 0x1F, 4);
	CHECK(cu_smbios_entry(mem_map, NULL, MEM_BASE + 0x100u, 0, &found) == 1);
	CHECK(found.address == MEM_BASE + 0x2000u && found.max_bytes == 0x134u && found.major == 2u);
	CHECK(cu_smbios_entry(mem_map, NULL, MEM_BASE + 0x100u, 1, &found) == 0);
}

/* ---- text and device paths ----------------------------------------------------------------------- */

static void wide(unsigned short *out, const char *s)
{
	while (*s) *out++ = (unsigned char)*s++;
	*out = 0;
}

static int wide_is(const unsigned short *w, const char *s)
{
	while (*s) if (*w++ != (unsigned char)*s++) return 0;
	return *w == 0;
}

static size_t add_file_node(unsigned char *p, size_t at, const char *path)
{
	size_t n = strlen(path), i, bytes = 4u + 2u * (n + 1u);
	p[at] = 4; p[at + 1] = 4; p[at + 2] = (unsigned char)bytes; p[at + 3] = (unsigned char)(bytes >> 8);
	for (i = 0; i <= n; i++) { p[at + 4 + 2 * i] = (unsigned char)path[i]; p[at + 5 + 2 * i] = 0; }
	return at + bytes;
}

static size_t add_end(unsigned char *p, size_t at)
{
	p[at] = 0x7F; p[at + 1] = 0xFF; p[at + 2] = 4; p[at + 3] = 0;
	return at + 4;
}

static void test_text(void)
{
	unsigned short opt[64], file[128], marker[128];
	unsigned char dp[256], out[512];
	struct cu_options o;
	char line[64];
	size_t at, bytes;

	wide(opt, "core-unlock.efi reset=fadt");
	cu_options_parse(opt, 64, &o);
	CHECK(o.reset == CU_RESET_FADT && o.unknown == 0);
	wide(opt, "reset=none");
	cu_options_parse(opt, 64, &o);
	CHECK(o.reset == CU_RESET_NONE);
	wide(opt, "  reset=warm  ");
	cu_options_parse(opt, 64, &o);
	CHECK(o.reset == CU_RESET_WARM && o.unknown == 0);
	wide(opt, "reset=cold wait=5");
	cu_options_parse(opt, 64, &o);
	CHECK(o.reset == CU_RESET_WARM && o.unknown == 2);
	wide(opt, "reset=fadt");
	cu_options_parse(opt, 5, &o);		/* the size ends the text before the value */
	CHECK(o.reset == CU_RESET_WARM && o.unknown == 0);
	cu_options_parse(NULL, 0, &o);
	CHECK(o.reset == CU_RESET_WARM);
	CHECK(cu_reset_method_name(CU_RESET_COUNT) != NULL);

	/* A device path: one hardware node, the file in two nodes, the end. */
	dp[0] = 1; dp[1] = 1; dp[2] = 6; dp[3] = 0; dp[4] = 0; dp[5] = 0;
	at = add_file_node(dp, 6, "\\EFI\\BC250");
	at = add_file_node(dp, at, "core-unlock.efi");
	at = add_end(dp, at);
	CHECK(cu_devpath_bytes(dp, sizeof(dp)) == at);
	CHECK(cu_devpath_file(dp, sizeof(dp), file, 128) == 1);
	CHECK(wide_is(file, "\\EFI\\BC250\\core-unlock.efi"));
	CHECK(cu_marker_path(file, marker, 128) == 1);
	CHECK(wide_is(marker, "\\EFI\\BC250\\core-unlock.mark"));
	CHECK(cu_marker_path(file, marker, 10) == 0);
	wide(opt, "no-directory.efi");
	CHECK(cu_marker_path(opt, marker, 128) == 0);
	CHECK(cu_devpath_bytes(dp, at - 1u) == 0);
	CHECK(cu_devpath_file(dp, sizeof(dp), file, 8) == 0);

	/* The Windows Boot Manager on the device of the image: the device path of the device handle, which is the
	 * hardware node and the end node. */
	add_end(dp, 6);
	wide(file, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi");
	bytes = cu_devpath_with_file(dp, sizeof(dp), file, out, sizeof(out));
	CHECK(bytes == 6u + 4u + 2u * 33u + 4u);
	CHECK(cu_devpath_bytes(out, bytes) == bytes);
	CHECK(out[0] == 1 && out[6] == 4 && out[7] == 4 && out[bytes - 4] == 0x7F && out[bytes - 3] == 0xFF);
	CHECK(cu_devpath_file(out, bytes, marker, 128) == 1 && wide_is(marker, "\\EFI\\Microsoft\\Boot\\bootmgfw.efi"));
	CHECK(cu_devpath_with_file(dp, sizeof(dp), file, out, 20) == 0);

	cu_format(line, sizeof(line), "%u %x %d %s %llx %%", 12u, 0xABu, -3, "ok", 0x1234ABCD5678ull);
	CHECK(!strcmp(line, "12 AB -3 ok 1234ABCD5678 %"));
	cu_format(line, 6, "%s", "truncated");
	CHECK(!strcmp(line, "trunc"));
}

int main(void)
{
	test_decide();
	test_marker_text();
	test_smu_sent();
	test_smu_failures();
	test_run();
	test_acpi();
	test_smbios();
	test_text();
	printf("core-unlock host tests: %u checks, %u failures\n", checks, failures);
	return failures ? 1 : 0;
}
