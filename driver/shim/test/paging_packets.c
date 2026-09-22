/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Packet check for driver/shim/bc250_sdma_paging.c (ADR 0008 stage D, docs/design/paging-node.md
 * section 4a): does bc250_sdma_paging_copy()/bc250_sdma_paging_fill() write exactly what
 * bc250_sdma_emit_copy_linear()/emit_fill() would (already checked against AMD's own field macros
 * by driver/shim/test/sdma_copy_packets.c, M95) into a plain caller-owned buffer, and does the room
 * check answer BC250_SDMA_PAGING_INSUFFICIENT - writing nothing - when the buffer is too small for
 * the operation, the shape DXGKDDI_BUILDPAGINGBUFFER needs for STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER.
 *
 * What BuildPagingBuffer itself does - resolving a DXGK_OPERATION_VIRTUAL_TRANSFER/VIRTUAL_FILL's
 * virtual addresses through VidMmTranslate(), finding the root through hSystemContext, and the
 * shadow-buffer copy for the DISPATCH_LEVEL submit path - is driver/kmd code with no host harness;
 * this file is the packet-shape half only, the same split sdma_copy_packets.c draws between "the
 * packets are right" (here) and "the hardware ran them" (BC250_ESCAPE_RUN_SDMACOPY on unit A).
 *
 * Links against driver/shim/bc250_sdma_paging.c, bc250_sdma_copy.c, bc250_sdma.c, bc250_ring.c and
 * bc250_nbio.c - the same files driver/kmd/build.ps1 compiles for the miniport (bc250_sdma_paging.c
 * added to that list alongside them) - plus a plain shim backend, the same shape
 * driver/shim/test/sdma_copy_packets.c supplies for the same reason: nothing here calls
 * bc250_sdma_setup()/hw_init(), so the register side of the backend is never exercised.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc250_sdma_paging.h"
#include "bc250_sdma.h"
#include "bc250_gmc.h"                    /* BC250_EINVAL */
#include "bc250_gfx.h"                    /* BC250_ENOMEM, pulled in the same way bc250_sdma.c pulls it */
#include "navi10_sdma_pkt_open.h"         /* the same imported header bc250_sdma_copy.c builds against */

/* ---------------------------------------------------------------------------------------------
 * A plain shim backend, cut down exactly as sdma_copy_packets.c's: nothing this file runs calls
 * bc250_sdma_setup()/hw_init()/init_golden_registers() or rings a doorbell, so the register and
 * doorbell sides exist only so the link has something to resolve RREG32/WREG32/wdoorbell* to.
 * ------------------------------------------------------------------------------------------- */

static u32 g_regs[1];

/* The device both entry points now take. Static, not a local, for the reason the header spells out
 * (facts M104): struct amdgpu_device is 0x5B00 bytes and a kernel stack is 24 KB. Nothing this file
 * runs reads a field of it; it exists so that ring->adev is a real, non-NULL device. */
static struct amdgpu_device g_adev;

int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain, unsigned int size,
                         unsigned int align, struct bc250_mem *out)
{
	(void)adev; (void)domain; (void)size; (void)align;
	memset(out, 0, sizeof(*out));
	return -12;
}

void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *m)
{
	(void)adev;
	if (m) memset(m, 0, sizeof(*m));
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
{ (void)adev; (void)index; (void)value; }
void bc250_shim_wdoorbell32(struct amdgpu_device *adev, unsigned int index, unsigned int value)
{ (void)adev; (void)index; (void)value; }

void bc250_shim_log(int level, void *dev, const char *fmt, ...)
{
	va_list ap;
	(void)level; (void)dev;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
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

static int all_zero(const u32 *buf, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (buf[i] != 0)
			return 0;
	return 1;
}

static int all_pattern(const u32 *buf, unsigned int count, u32 pattern)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (buf[i] != pattern)
			return 0;
	return 1;
}

#define TEST_BUF_DWORDS 4096

/* ---------------------------------------------------------------------------------------------
 * Case 1: TRANSFER_VIRTUAL of 3 pages (3 * PAGE_SIZE = 12288 bytes) - one packet, well under
 * copy_max_bytes (0x400000).
 * ------------------------------------------------------------------------------------------- */

static void case_transfer_three_pages(void)
{
	static u32 buf[TEST_BUF_DWORDS];
	u64 src = 0x0000002000001000ULL, dst = 0x0000003000002000ULL;
	unsigned int bytes = 3u * 4096u;
	unsigned int written = 0;
	u32 want[7];

	memset(buf, 0, sizeof(buf));

	printf("\n-- TRANSFER_VIRTUAL, 3 pages, %u bytes --\n", bytes);
	check(bc250_sdma_paging_copy(&g_adev, buf, TEST_BUF_DWORDS, src, dst, bytes, &written) == BC250_SDMA_PAGING_OK,
	      "bc250_sdma_paging_copy succeeds with room to spare");
	check(written == 7, "one packet is 7 dwords, same as bc250_sdma_copy_linear_size(bytes)");

	want[0] = SDMA_PKT_HEADER_OP(SDMA_OP_COPY) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR);
	want[1] = bytes - 1u;
	want[2] = 0;
	want[3] = (u32)(src & 0xFFFFFFFFu);
	want[4] = (u32)(src >> 32);
	want[5] = (u32)(dst & 0xFFFFFFFFu);
	want[6] = (u32)(dst >> 32);
	check_dwords(buf, want, 7, "the packet matches bc250_sdma_emit_copy_linear() field for field");
	check(all_zero(buf + 7, 8), "nothing written past the packet");
}

/* ---------------------------------------------------------------------------------------------
 * Case 2: FILL_VIRTUAL of 2 pages (2 * PAGE_SIZE = 8192 bytes) - one packet.
 * ------------------------------------------------------------------------------------------- */

static void case_fill_two_pages(void)
{
	static u32 buf[TEST_BUF_DWORDS];
	u64 dst = 0x0000004000003000ULL;
	u32 pattern = 0x00000000u;         /* DXGK_OPERATION_VIRTUAL_FILL zeroes a range on this driver */
	unsigned int bytes = 2u * 4096u;
	unsigned int written = 0;
	u32 want[5];

	memset(buf, 0, sizeof(buf));

	printf("\n-- FILL_VIRTUAL, 2 pages, %u bytes --\n", bytes);
	check(bc250_sdma_paging_fill(&g_adev, buf, TEST_BUF_DWORDS, dst, pattern, bytes, &written) == BC250_SDMA_PAGING_OK,
	      "bc250_sdma_paging_fill succeeds with room to spare");
	check(written == 5, "one packet is 5 dwords, same as bc250_sdma_fill_size(bytes)");

	want[0] = SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL);
	want[1] = (u32)(dst & 0xFFFFFFFFu);
	want[2] = (u32)(dst >> 32);
	want[3] = pattern;
	want[4] = bytes - 1u;
	check_dwords(buf, want, 5, "the packet matches bc250_sdma_emit_fill() field for field");
	check(all_zero(buf + 5, 10), "nothing written past the packet");
}

/* ---------------------------------------------------------------------------------------------
 * Case 3: insufficient buffer - the room BuildPagingBuffer has left is smaller than the operation,
 * the shape STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER needs. Nothing is written either way.
 * ------------------------------------------------------------------------------------------- */

static void case_insufficient(void)
{
	static u32 buf[TEST_BUF_DWORDS];
	u64 src = 0x0000005000000000ULL, dst = 0x0000006000000000ULL;
	unsigned int written = 0;

	memset(buf, 0xCC, sizeof(buf));    /* not zero, so a stray write would be caught either way */

	printf("\n-- insufficient buffer --\n");
	check(bc250_sdma_paging_copy(&g_adev, buf, 3, src, dst, 12288, &written) == BC250_SDMA_PAGING_INSUFFICIENT,
	      "3 dwords of room refuses a 7-dword TRANSFER");
	check(written == 16, "reports the SDMA-aligned dword count the operation needs (7 rounded up to 16)");
	check(all_pattern(buf, TEST_BUF_DWORDS, 0xCCCCCCCCu), "the whole buffer is untouched (still 0xCC)");

	memset(buf, 0xCC, sizeof(buf));
	written = 0;
	check(bc250_sdma_paging_fill(&g_adev, buf, 2, dst, 0, 8192, &written) == BC250_SDMA_PAGING_INSUFFICIENT,
	      "2 dwords of room refuses a 5-dword FILL");
	check(written == 16, "reports the SDMA-aligned dword count the operation needs (5 rounded up to 16)");
	check(all_pattern(buf, TEST_BUF_DWORDS, 0xCCCCCCCCu), "the whole buffer is untouched (still 0xCC)");

	memset(buf, 0xCC, sizeof(buf));
	written = 0;
	check(bc250_sdma_paging_copy(&g_adev, buf, 0, src, dst, 12288, &written) == BC250_SDMA_PAGING_INSUFFICIENT,
	      "zero dwords of room also refuses");
	check(all_pattern(buf, TEST_BUF_DWORDS, 0xCCCCCCCCu), "and still writes nothing");
}

/* ---------------------------------------------------------------------------------------------
 * Case 4: refusals - a bad argument writes nothing and answers EINVAL, not INSUFFICIENT.
 * ------------------------------------------------------------------------------------------- */

static void case_refusals(void)
{
	static u32 buf[TEST_BUF_DWORDS];
	unsigned int written = 0xDEADBEEFu;

	memset(buf, 0, sizeof(buf));

	printf("\n-- refusals --\n");
	check(bc250_sdma_paging_copy(&g_adev, NULL, TEST_BUF_DWORDS, 0x1000, 0x2000, 4096, &written) == BC250_SDMA_PAGING_EINVAL,
	      "bc250_sdma_paging_copy refuses a NULL buffer");
	check(bc250_sdma_paging_copy(NULL, buf, TEST_BUF_DWORDS, 0x1000, 0x2000, 4096, &written) == BC250_SDMA_PAGING_EINVAL,
	      "bc250_sdma_paging_copy refuses a NULL device (M104: the caller's live adev, never a local one)");
	check(bc250_sdma_paging_fill(NULL, buf, TEST_BUF_DWORDS, 0x1000, 0, 4096, &written) == BC250_SDMA_PAGING_EINVAL,
	      "bc250_sdma_paging_fill refuses a NULL device");
	check(bc250_sdma_paging_copy(&g_adev, buf, TEST_BUF_DWORDS, 0x1000, 0x2000, 0, &written) == BC250_SDMA_PAGING_EINVAL,
	      "bc250_sdma_paging_copy refuses 0 bytes");
	check(bc250_sdma_paging_fill(&g_adev, buf, TEST_BUF_DWORDS, 0x1000, 0, 0, &written) == BC250_SDMA_PAGING_EINVAL,
	      "bc250_sdma_paging_fill refuses 0 bytes");
	check(all_zero(buf, TEST_BUF_DWORDS), "none of the refusals wrote anything");
}

/* ------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			g_verbose = 1;

	printf("Paging node packets (ADR 0008 stage D): driver/shim/bc250_sdma_paging.c against\n");
	printf("bc250_sdma_emit_copy_linear()/emit_fill() (driver/shim/bc250_sdma_copy.c, M95), dword for\n");
	printf("dword, plus the room check BuildPagingBuffer's STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER needs.\n");

	case_transfer_three_pages();
	case_fill_two_pages();
	case_insufficient();
	case_refusals();

	printf("\n== verdict ==\n");
	printf("  %u checks, %u failures\n", g_checks, g_failures);
	printf("  %s\n", g_failures == 0 ? "PASS" : "FAIL");
	return g_failures == 0 ? 0 : 1;
}
