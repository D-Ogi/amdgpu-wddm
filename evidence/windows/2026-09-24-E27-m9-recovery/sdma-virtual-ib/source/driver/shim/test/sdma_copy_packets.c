/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Packet check for driver/shim/bc250_sdma_copy.c (ADR 0013): does bc250_sdma_emit_copy_linear()
 * and bc250_sdma_emit_fill() write exactly the dwords amdgpu's sdma_v5_0_emit_copy_buffer() and
 * sdma_v5_0_emit_fill_buffer() would (driver/amdgpu-import/reference/sdma_v5_0.c:2018 and :2045),
 * split into packets the way amdgpu_copy_buffer()/amdgpu_ttm_fill_mem() (amdgpu_ttm.c) split them.
 *
 * Nothing here touches a register or a trace: bc250_sdma_copy.c emits no register write and there
 * is no window of unit A's E13 trace with a copy or a fill in it (M6's bring-up never asked SDMA to
 * move anything). What is checked instead is that the dwords match AMD's own field macros -
 * navi10_sdma_pkt_open.h, the same imported header bc250_sdma_copy.c builds packets with - dword
 * for dword, including a request that has to split into two packets. That is a positive control in
 * the sense ADR 0013 uses the phrase: it says the packet builder agrees with the header it is built
 * from, not that the packet does anything on real hardware. The escape (BC250_ESCAPE_RUN_SDMACOPY)
 * is the one that runs a copy on unit A and reads the destination back from VRAM.
 *
 * bc250_sdma_copy_test() is checked too, end to end against a memory ring: fill, then copy, then
 * fence packets land in the ring in that order and nothing else, amdgpu_ring_commit() rings the
 * doorbell once, and the driver's own bc250_sdma_fence_addr()/bc250_sdma_fence_read() see whatever
 * is written into the fence slot - there being no hardware here to write it itself.
 *
 * Links against driver/shim/bc250_sdma_copy.c, bc250_sdma.c, bc250_ring.c and bc250_nbio.c, the same
 * four files driver/kmd/build.ps1 compiles for the miniport (bc250_sdma_copy.c added to that list
 * alongside them). bc250_sdma.c pulls in golden-register tables and SOC15 register macros this file
 * never exercises - nothing here calls bc250_sdma_setup()/hw_init() - but the object still needs
 * every external symbol it names resolved to link, so this file supplies the same shim backend
 * driver/shim/test/sdma_faults.c does, cut down to a plain working allocator and register file since
 * nothing here injects a fault.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc250_sdma.h"
#include "bc250_gmc.h"                    /* BC250_EINVAL */
#include "bc250_gfx.h"                    /* BC250_ENOMEM, pulled in the same way bc250_sdma.c pulls it */
#include "navi10_sdma_pkt_open.h"         /* the same imported header bc250_sdma_copy.c builds against */

/* ---------------------------------------------------------------------------------------------
 * A plain shim backend: malloc for memory, a no-op register file. bc250_sdma.c is linked for
 * bc250_sdma_fence_size()/bc250_sdma_emit_fence(), which bc250_sdma_copy_test() calls, but nothing
 * this file runs calls bc250_sdma_setup()/hw_init()/init_golden_registers(), so the register side of
 * this backend is never exercised - it exists only so the link has something to resolve
 * RREG32/WREG32 to. bc250_shim_wdoorbell64() is not a no-op: it is the one call this file checks,
 * to say that bc250_sdma_copy_test() rang the doorbell exactly once.
 * ------------------------------------------------------------------------------------------- */

#define TEST_MAX_OBJ 8

struct test_obj { void *cpu; u64 mc; unsigned int size; int live; };
static struct test_obj g_objs[TEST_MAX_OBJ];
static u64 g_next_mc = 0x0000003000000000ULL;

static u32 g_regs[1];              /* never read for real; present only so RREG32/WREG32 compile */
static unsigned int g_doorbell_count;
static unsigned int g_doorbell_index;
static u64 g_doorbell_value;

int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain, unsigned int size,
                         unsigned int align, struct bc250_mem *out)
{
	unsigned int i;
	(void)adev; (void)domain; (void)align;

	memset(out, 0, sizeof(*out));
	for (i = 0; i < TEST_MAX_OBJ; i++)
		if (!g_objs[i].live)
			break;
	if (i == TEST_MAX_OBJ)
		return -12;
	g_objs[i].cpu = calloc(1, size ? size : 1);
	if (g_objs[i].cpu == NULL)
		return -12;
	g_objs[i].mc = g_next_mc;
	g_objs[i].size = size;
	g_objs[i].live = 1;
	g_next_mc += (u64)size + 0x1000u;

	out->cpu = g_objs[i].cpu;
	out->mc = g_objs[i].mc;
	out->size = size;
	return 0;
}

void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *m)
{
	unsigned int i;
	(void)adev;

	if (m == NULL || (m->cpu == NULL && m->mc == 0 && m->size == 0))
		return;
	for (i = 0; i < TEST_MAX_OBJ; i++) {
		if (!g_objs[i].live || g_objs[i].cpu != m->cpu)
			continue;
		free(g_objs[i].cpu);
		g_objs[i].live = 0;
		break;
	}
	memset(m, 0, sizeof(*m));
}

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index)
{
	(void)adev;
	return dword_index < (sizeof(g_regs) / sizeof(g_regs[0])) ? g_regs[dword_index] : 0;
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value)
{
	(void)adev;
	if (dword_index < (sizeof(g_regs) / sizeof(g_regs[0])))
		g_regs[dword_index] = value;
}

void bc250_shim_udelay(unsigned int usec) { (void)usec; }

void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index, unsigned long long value)
{
	(void)adev;
	g_doorbell_count++;
	g_doorbell_index = index;
	g_doorbell_value = value;
}

void bc250_shim_wdoorbell32(struct amdgpu_device *adev, unsigned int index, unsigned int value)
{
	(void)adev; (void)index; (void)value;
}

void bc250_shim_log(int level, void *dev, const char *fmt, ...)
{
	va_list ap;
	(void)level; (void)dev;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* Packet controls never enter RLC reset lifecycle code. */
int bc250_gfx_rlc_safe_enter(struct amdgpu_device *adev, bool *requested)
{
    (void)adev; (void)requested; abort();
}
void bc250_gfx_rlc_safe_exit(struct amdgpu_device *adev, bool requested)
{
    (void)adev; (void)requested; abort();
}

/* ---------------------------------------------------------------------------------------------
 * Expectations
 * ------------------------------------------------------------------------------------------- */

static unsigned int g_checks, g_failures;
static int g_verbose;

static void check(int ok, const char *what)
{
	g_checks++;
	if (ok) {
		if (g_verbose)
			printf("    ok    %s\n", what);
		return;
	}
	g_failures++;
	printf("    FAIL  %s\n", what);
}

static void check_dwords(const u32 *got, const u32 *want, unsigned int count, const char *what)
{
	unsigned int i;
	int ok = 1;

	for (i = 0; i < count; i++)
		if (got[i] != want[i])
			ok = 0;
	check(ok, what);
	if (!ok || g_verbose) {
		for (i = 0; i < count; i++)
			printf("      dw %2u  got 0x%08X  want 0x%08X%s\n", i, got[i], want[i],
			       got[i] != want[i] ? "  <-- mismatch" : "");
	}
}

/* ---------------------------------------------------------------------------------------------
 * A ring in plain memory. No adev->sdma instance, no doorbell aperture, no writeback slot: the
 * emitters under test read none of those, only ring->ring/wptr/buf_mask/ptr_mask/funcs.
 * ------------------------------------------------------------------------------------------- */

#define TEST_RING_DWORDS 4096

static const struct amdgpu_ring_funcs g_sdma_funcs = { AMDGPU_RING_TYPE_SDMA, 0xfu,
                                                       SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP) };
static const struct amdgpu_ring_funcs g_gfx_funcs = { AMDGPU_RING_TYPE_GFX, 0x3fu, 0 };

static void ring_init(struct amdgpu_ring *ring, struct amdgpu_device *adev, const struct amdgpu_ring_funcs *funcs,
                      u32 *buffer)
{
	memset(ring, 0, sizeof(*ring));
	ring->adev = adev;
	ring->funcs = funcs;
	ring->ring = buffer;
	ring->ring_size = TEST_RING_DWORDS * 4u;
	ring->buf_mask = TEST_RING_DWORDS - 1u;
	ring->ptr_mask = 0xffffffffffffffffULL;
	ring->max_dw = TEST_RING_DWORDS;
	ring->use_doorbell = 1;
	ring->doorbell_index = 0x200;
	ring->me = 0;
}

#include "sdma-ib-reference.h"

static void case_indirect_buffers(void)
{
    static u32 actual[TEST_RING_DWORDS], expected[TEST_RING_DWORDS];
    static const u32 lengths[] = {1u, 8u, 1024u, SDMA_PKT_INDIRECT_IB_SIZE_ib_size_mask};
    struct amdgpu_device adev;
    struct amdgpu_ring ring, oracle;
    unsigned int pos, vmid, variant;
    printf("\n-- INDIRECT: executable AMD reference, VMIDs, wrap, submit fence --\n");
    memset(&adev, 0, sizeof(adev));
    for (pos = 0; pos < 32; pos++) for (vmid = 0; vmid < 16; vmid++)
    for (variant = 0; variant < 4; variant++) {
        struct amdgpu_job job = {vmid};
        struct amdgpu_ib ib = {0x0000001234560020ULL, lengths[variant]};
        u64 start = pos < 16 ? pos : pos < 24 ? TEST_RING_DWORDS - 8u + pos - 16u :
                                               0x100000000ULL + pos - 24u;
        unsigned int count, before;
        reference_csa = (variant & 1u) ? 0x0000005678000000ULL : 0;
        memset(actual, 0xcc, sizeof(actual)); memset(expected, 0xcc, sizeof(expected));
        ring_init(&ring, &adev, &g_sdma_funcs, actual);
        ring_init(&oracle, &adev, &g_sdma_funcs, expected);
        ring.wptr = oracle.wptr = start;
        count = bc250_sdma_ib_size(&ring);
        reference_sdma_emit_ib(&oracle, &job, &ib, 0);
        check(bc250_sdma_emit_ib(&ring, ib.gpu_addr, ib.length_dw, vmid, reference_csa) == 0,
              "valid indirect buffer emits");
        check(ring.wptr == oracle.wptr && ring.wptr - start == count && !(ring.wptr & 7u),
              "reference size and eight-word end alignment");
        check(memcmp(actual, expected, sizeof(actual)) == 0, "all packet words match imported AMD function");
        memset(actual, 0xcc, sizeof(actual)); memset(expected, 0xcc, sizeof(expected));
        ring.wptr = oracle.wptr = start;
        check(amdgpu_ring_alloc(&oracle, count + bc250_sdma_fence_size(&oracle, AMDGPU_FENCE_FLAG_INT)) == 0,
              "oracle reservation fits");
        reference_sdma_emit_ib(&oracle, &job, &ib, 0);
        check(bc250_sdma_emit_fence(&oracle, 0x0000006789000000ULL, 73, AMDGPU_FENCE_FLAG_INT) == 0,
              "oracle outer fence emits");
        amdgpu_ring_commit(&oracle);
        before = g_doorbell_count;
        check(bc250_sdma_submit_ib(&ring, ib.gpu_addr, ib.length_dw, vmid, reference_csa,
                                 0x0000006789000000ULL, 73, AMDGPU_FENCE_FLAG_INT) == 0,
              "indirect submission commits");
        check(ring.wptr == oracle.wptr && memcmp(actual, expected, sizeof(actual)) == 0,
              "submission preserves IB then fence then commit padding");
        check(ring.count_dw >= 0, "reservation covers final commit padding from every tested start");
        check(g_doorbell_count == before + 1 && g_doorbell_value == ring.wptr * 4u,
              "one byte-valued doorbell publication");
    }
    ring_init(&ring, &adev, &g_sdma_funcs, actual);
    memset(actual, 0xcc, sizeof(actual)); memcpy(expected, actual, sizeof(actual));
    check(bc250_sdma_emit_ib(&ring, 33, 8, 2, 0) == BC250_EINVAL, "unaligned IB is not rounded down");
    check(bc250_sdma_emit_ib(&ring, 32, 8, 16, 0) == BC250_EINVAL, "VMID is not truncated");
    check(bc250_sdma_emit_ib(&ring, 32, SDMA_PKT_INDIRECT_IB_SIZE_ib_size_mask + 1u, 2, 0) == BC250_EINVAL,
          "IB size is not truncated");
    check(ring.wptr == 0 && memcmp(actual, expected, sizeof(actual)) == 0, "refused packet publishes no bytes");
}

/* bc250_sdma_copy.c keeps copy_max_bytes/fill_max_bytes as its own file-local constants (both
 * 0x400000, sdma_v5_0_buffer_funcs) rather than exporting them - a caller only ever needs the
 * dword count bc250_sdma_copy_linear_size()/bc250_sdma_fill_size() already give it. This file's
 * own copy of the number, for building the split case below, is checked against the header's own
 * COUNT mask in case_copy_split() rather than trusted on its own. */
#define BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES 0x400000u

/* ---------------------------------------------------------------------------------------------
 * Case 1: one copy packet, well under copy_max_bytes (0x400000).
 * ------------------------------------------------------------------------------------------- */

static void case_copy_one_packet(void)
{
	static u32 buf[TEST_RING_DWORDS];
	struct amdgpu_device adev;
	struct amdgpu_ring ring;
	u64 src = 0x0000001234560000ULL, dst = 0x0000005678900000ULL;
	unsigned int bytes = 4096;
	u32 want[7];

	memset(&adev, 0, sizeof(adev));
	ring_init(&ring, &adev, &g_sdma_funcs, buf);

	printf("\n-- copy, one packet, %u bytes --\n", bytes);
	check(bc250_sdma_copy_linear_size(bytes) == 7, "bc250_sdma_copy_linear_size: one packet is 7 dwords");
	check(bc250_sdma_emit_copy_linear(&ring, src, dst, bytes) == 0, "bc250_sdma_emit_copy_linear succeeds");
	check(ring.wptr == 7, "the write pointer advanced by exactly 7 dwords");

	/* sdma_v5_0_emit_copy_buffer() (reference/sdma_v5_0.c:2024-2032), copy_flags 0 (no TMZ):
	 *   dw0  header: SDMA_OP_COPY in bits 7:0, SDMA_SUBOP_COPY_LINEAR in bits 15:8, TMZ (bit 18) clear
	 *   dw1  byte_count - 1 (the COUNT field starts at bit 0 of its own dword)
	 *   dw2  src/dst endian swap: 0, this engine never asks for one
	 *   dw3  src_mc bits 31:0
	 *   dw4  src_mc bits 63:32
	 *   dw5  dst_mc bits 31:0
	 *   dw6  dst_mc bits 63:32
	 */
	want[0] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR);
	want[1] = bytes - 1u;
	want[2] = 0;
	want[3] = (u32)(src & 0xFFFFFFFFu);
	want[4] = (u32)(src >> 32);
	want[5] = (u32)(dst & 0xFFFFFFFFu);
	want[6] = (u32)(dst >> 32);
	check_dwords(buf, want, 7, "the copy packet matches sdma_v5_0_emit_copy_buffer() field for field");
}

/* ---------------------------------------------------------------------------------------------
 * Case 2: one fill packet.
 * ------------------------------------------------------------------------------------------- */

static void case_fill_one_packet(void)
{
	static u32 buf[TEST_RING_DWORDS];
	struct amdgpu_device adev;
	struct amdgpu_ring ring;
	u64 dst = 0x0000004242000000ULL;
	u32 pattern = 0xCAFEF00Du;
	unsigned int bytes = 65536;
	u32 want[5];

	memset(&adev, 0, sizeof(adev));
	ring_init(&ring, &adev, &g_sdma_funcs, buf);

	printf("\n-- fill, one packet, %u bytes --\n", bytes);
	check(bc250_sdma_fill_size(bytes) == 5, "bc250_sdma_fill_size: one packet is 5 dwords");
	check(bc250_sdma_emit_fill(&ring, dst, pattern, bytes) == 0, "bc250_sdma_emit_fill succeeds");
	check(ring.wptr == 5, "the write pointer advanced by exactly 5 dwords");

	/* sdma_v5_0_emit_fill_buffer() (reference/sdma_v5_0.c:2050-2054):
	 *   dw0  header: SDMA_OP_CONST_FILL in bits 7:0; upstream sets no sub_op and no FILLSIZE, so both
	 *        stay 0 (byte granularity), which is bits 15:8 and 31:30 of the same dword left at 0
	 *   dw1  dst_mc bits 31:0
	 *   dw2  dst_mc bits 63:32
	 *   dw3  the fill pattern, verbatim (src_data)
	 *   dw4  byte_count - 1
	 */
	want[0] = SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL);
	want[1] = (u32)(dst & 0xFFFFFFFFu);
	want[2] = (u32)(dst >> 32);
	want[3] = pattern;
	want[4] = bytes - 1u;
	check_dwords(buf, want, 5, "the fill packet matches sdma_v5_0_emit_fill_buffer() field for field");
}

/* ---------------------------------------------------------------------------------------------
 * Case 3: a copy that has to split. copy_max_bytes is 0x400000 (SDMA_PKT_COPY_LINEAR_COUNT's 22-bit
 * count field, byte_count - 1, tops out there); this asks for one byte more, so amdgpu_copy_buffer()'s
 * DIV_ROUND_UP gives two packets, the first carrying the whole max and the second the leftover byte,
 * both addresses advanced by what the first packet moved.
 * ------------------------------------------------------------------------------------------- */

static void case_copy_split(void)
{
	static u32 buf[TEST_RING_DWORDS];
	struct amdgpu_device adev;
	struct amdgpu_ring ring;
	u64 src = 0x0000002000000000ULL, dst = 0x0000003000000000ULL;
	unsigned int bytes = BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES + 1u;
	u32 want[14];

	memset(&adev, 0, sizeof(adev));
	ring_init(&ring, &adev, &g_sdma_funcs, buf);

	printf("\n-- copy, split into two packets, %u bytes --\n", bytes);
	check(SDMA_PKT_COPY_LINEAR_COUNT_count_mask == 0x003FFFFFu,
	      "the header's own COUNT mask is 22 bits, i.e. 0x400000 bytes - not retyped, checked");
	check(bc250_sdma_copy_linear_size(bytes) == 14, "bc250_sdma_copy_linear_size: two packets is 14 dwords");
	check(bc250_sdma_emit_copy_linear(&ring, src, dst, bytes) == 0, "bc250_sdma_emit_copy_linear succeeds");
	check(ring.wptr == 14, "the write pointer advanced by exactly 14 dwords");

	/* First packet: the whole max chunk, addresses as given. */
	want[0] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR);
	want[1] = BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES - 1u;
	want[2] = 0;
	want[3] = (u32)(src & 0xFFFFFFFFu);
	want[4] = (u32)(src >> 32);
	want[5] = (u32)(dst & 0xFFFFFFFFu);
	want[6] = (u32)(dst >> 32);
	/* Second packet: the one leftover byte, both addresses advanced by the first chunk. */
	want[7] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR);
	want[8] = 0;                    /* 1 byte - 1 */
	want[9] = 0;
	want[10] = (u32)((src + BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES) & 0xFFFFFFFFu);
	want[11] = (u32)((src + BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES) >> 32);
	want[12] = (u32)((dst + BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES) & 0xFFFFFFFFu);
	want[13] = (u32)((dst + BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES) >> 32);
	check_dwords(buf, want, 14, "both packets match, split at copy_max_bytes with the remainder second");
}

/* Fill splits the same way; one case is enough to confirm the loop is shared, not that the fill
 * arithmetic differs from the copy's - it does not (amdgpu_ttm_fill_mem() vs. amdgpu_copy_buffer(),
 * both DIV_ROUND_UP over their own max_bytes). */
static void case_fill_split(void)
{
	static u32 buf[TEST_RING_DWORDS];
	struct amdgpu_device adev;
	struct amdgpu_ring ring;
	u64 dst = 0x0000004000000000ULL;
	u32 pattern = 0x11111111u;
	unsigned int bytes = BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES + 4096u;
	u32 want[10];

	memset(&adev, 0, sizeof(adev));
	ring_init(&ring, &adev, &g_sdma_funcs, buf);

	printf("\n-- fill, split into two packets, %u bytes --\n", bytes);
	check(bc250_sdma_fill_size(bytes) == 10, "bc250_sdma_fill_size: two packets is 10 dwords");
	check(bc250_sdma_emit_fill(&ring, dst, pattern, bytes) == 0, "bc250_sdma_emit_fill succeeds");

	want[0] = SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL);
	want[1] = (u32)(dst & 0xFFFFFFFFu);
	want[2] = (u32)(dst >> 32);
	want[3] = pattern;
	want[4] = BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES - 1u;
	want[5] = SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL);
	want[6] = (u32)((dst + BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES) & 0xFFFFFFFFu);
	want[7] = (u32)((dst + BC250_SDMA_COPY_LINEAR_TEST_MAX_BYTES) >> 32);
	want[8] = pattern;
	want[9] = 4096u - 1u;
	check_dwords(buf, want, 10, "both packets match, split at fill_max_bytes with the remainder second");
}

/* ---------------------------------------------------------------------------------------------
 * Case 4: refusals write nothing.
 * ------------------------------------------------------------------------------------------- */

static void case_refusals(void)
{
	static u32 buf[TEST_RING_DWORDS];
	struct amdgpu_device adev;
	struct amdgpu_ring sdma_ring, gfx_ring;

	memset(&adev, 0, sizeof(adev));
	ring_init(&sdma_ring, &adev, &g_sdma_funcs, buf);
	ring_init(&gfx_ring, &adev, &g_gfx_funcs, buf);

	printf("\n-- refusals --\n");
	check(bc250_sdma_emit_copy_linear(&sdma_ring, 0x1000, 0x2000, 0) == BC250_EINVAL,
	      "emit_copy_linear refuses 0 bytes");
	check(sdma_ring.wptr == 0, "and writes nothing");
	check(bc250_sdma_emit_fill(&sdma_ring, 0x1000, 0, 0) == BC250_EINVAL, "emit_fill refuses 0 bytes");
	check(sdma_ring.wptr == 0, "and writes nothing");
	check(bc250_sdma_emit_copy_linear(&gfx_ring, 0x1000, 0x2000, 4096) == BC250_EINVAL,
	      "emit_copy_linear refuses a non-SDMA ring");
	check(gfx_ring.wptr == 0, "and writes nothing");
	check(bc250_sdma_emit_fill(&gfx_ring, 0x1000, 0, 4096) == BC250_EINVAL, "emit_fill refuses a non-SDMA ring");
	check(gfx_ring.wptr == 0, "and writes nothing");
	check(bc250_sdma_copy_linear_size(0) == 0, "bc250_sdma_copy_linear_size(0) is 0");
	check(bc250_sdma_fill_size(0) == 0, "bc250_sdma_fill_size(0) is 0");
}

/* ---------------------------------------------------------------------------------------------
 * Case 5: bc250_sdma_copy_test() end to end against the memory ring - fill, copy, fence, one
 * doorbell, in that order, nothing else in the ring.
 * ------------------------------------------------------------------------------------------- */

static void case_copy_test(void)
{
	static u32 buf[TEST_RING_DWORDS];
	struct amdgpu_device adev;
	struct amdgpu_ring ring;
	u64 src = 0x0000005000000000ULL, dst = 0x0000006000000000ULL, fence_addr;
	unsigned int bytes = 8192;
	u32 pattern = 0xA5A5A5A5u, seq = 42;
	unsigned int fill_dw, copy_dw, fence_dw;

	memset(&adev, 0, sizeof(adev));
	ring_init(&ring, &adev, &g_sdma_funcs, buf);

	printf("\n-- bc250_sdma_copy_test: fill, copy, fence, one doorbell --\n");
	check(bc250_sdma_fence_page_alloc(&adev) == 0, "the fence page allocates (this file's stub backend)");
	fence_addr = bc250_sdma_fence_addr(&adev, 2);       /* slot 2: the first fence slot, as bc250_sdma.c reserves it */
	check(fence_addr != 0, "and hands back a nonzero GPU address for slot 2");

	g_doorbell_count = 0;
	check(bc250_sdma_copy_test(&ring, src, dst, bytes, pattern, fence_addr, seq, AMDGPU_FENCE_FLAG_INT) == 0,
	      "bc250_sdma_copy_test succeeds");

	fill_dw = bc250_sdma_fill_size(bytes);
	copy_dw = bc250_sdma_copy_linear_size(bytes);
	fence_dw = bc250_sdma_fence_size(&ring, AMDGPU_FENCE_FLAG_INT);
	check(fill_dw == 5 && copy_dw == 7, "one fill packet and one copy packet at this size");
	printf("    note  fill %u dw, copy %u dw, fence %u dw = %u dw before padding\n",
	       fill_dw, copy_dw, fence_dw, fill_dw + copy_dw + fence_dw);

	check(buf[0] == SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL), "dword 0 is the fill packet's header");
	check(buf[fill_dw] == (SDMA_PKT_HEADER_OP(SDMA_OP_COPY) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR)),
	      "the copy packet's header follows it immediately");
	check(buf[fill_dw + 1] == bytes - 1u, "the copy packet's byte count is the request, unsplit at this size");
	check((buf[fill_dw + copy_dw] & 0xFFu) == SDMA_OP_FENCE, "the fence packet's header follows the copy");
	check(g_doorbell_count == 1, "amdgpu_ring_commit rang the doorbell exactly once");
	check(g_doorbell_index == ring.doorbell_index, "at this ring's doorbell index");
	check(g_doorbell_value == (ring.wptr << 2), "carrying the write pointer in bytes (SDMA's own convention)");

	/* Nothing on this host ring actually executes the copy: there is no engine here to fetch it. What
	 * BC250_ESCAPE_RUN_SDMACOPY checks on unit A is that the destination holds the pattern after the
	 * hardware runs these same packets; here the check is only that the packets are the right ones and
	 * that the driver's own fence bookkeeping (bc250_sdma_fence_addr/_read) agrees with what a real
	 * engine's WRITE_LINEAR/FENCE would land in. */
	{
		volatile u64 *slot = (volatile u64 *)((char *)adev.sdma.fence_mem.cpu + 2u * 8u);
		*slot = seq;
		check(bc250_sdma_fence_read(&adev, 2) == seq, "the fence slot reads back whatever was written into it");
	}

	bc250_sdma_fence_page_free(&adev);
}

/* ------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			g_verbose = 1;

	printf("SDMA copy/fill packets: driver/shim/bc250_sdma_copy.c against AMD's own field macros\n");
	printf("(navi10_sdma_pkt_open.h) and against sdma_v5_0_emit_copy_buffer()/emit_fill_buffer()\n");
	printf("(driver/amdgpu-import/reference/sdma_v5_0.c), dword for dword.\n");

	case_copy_one_packet();
	case_fill_one_packet();
	case_copy_split();
	case_fill_split();
	case_refusals();
	case_copy_test();
	case_indirect_buffers();

	printf("\n== verdict ==\n");
	printf("  %u checks, %u failures\n", g_checks, g_failures);
	printf("  %s\n", g_failures == 0 ? "PASS" : "FAIL");
	return g_failures == 0 ? 0 : 1;
}
