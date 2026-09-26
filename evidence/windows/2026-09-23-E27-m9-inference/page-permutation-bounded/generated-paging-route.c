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

static void case_write_ptes(void)
{
	u32 buf[32];
	const u64 values[3] = {0x123450067ULL, 0xABCDE0067ULL, 0};
	unsigned int written, cap;
	/* Golden payload: low/high words remain in OS page order, including an invalid
	 * zero PTE for cleanup. Address/count/opcode come from upstream packet fields. */
	const u32 expected[10] = {
		SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) | SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR),
		0x00002000, 0x00000001, 5,
		0x23450067, 0x00000001, 0xBCDE0067, 0x0000000A, 0, 0
	};
	memset(buf, 0xCC, sizeof(buf));
	check(bc250_sdma_paging_write_ptes(&g_adev,buf,32,0x100002000ULL,values,3,&written)==BC250_SDMA_PAGING_OK,
	      "scattered PTE WRITE_LINEAR builds");
	check(written==10 && memcmp(buf,expected,sizeof(expected))==0,"PTE packet equals independent golden words");
	check(all_pattern(buf+10,22,0xCCCCCCCCu),"PTE command leaves trailing guard untouched");
	for(cap=0;cap<16;cap++) {
		memset(buf,0xCC,sizeof(buf));
		check(bc250_sdma_paging_write_ptes(&g_adev,buf,cap,0x2000,values,3,&written)==BC250_SDMA_PAGING_INSUFFICIENT && written==16,
		      "PTE command respects aligned ring reservation");
		check(all_pattern(buf,32,0xCCCCCCCCu),"PTE insufficient capacity writes nothing");
	}
	check(bc250_sdma_paging_write_ptes(&g_adev,buf,16,0x2000,values,3,&written)==BC250_SDMA_PAGING_OK && written==10,
	      "PTE exact aligned capacity succeeds");
	memset(buf,0xCC,sizeof(buf));
	check(bc250_sdma_paging_write_ptes(&g_adev,buf,32,0x2001,values,3,&written)==BC250_SDMA_PAGING_EINVAL && !written,"PTE unaligned destination rejected");
	check(bc250_sdma_paging_write_ptes(&g_adev,buf,32,~(u64)7,values,3,&written)==BC250_SDMA_PAGING_EINVAL,"PTE destination overflow rejected");
	check(bc250_sdma_paging_write_ptes(&g_adev,buf,32,0x2000,values,0,&written)==BC250_SDMA_PAGING_EINVAL,"PTE empty list rejected");
	check(bc250_sdma_paging_write_ptes(&g_adev,buf,32,0x2000,values,0x80001,&written)==BC250_SDMA_PAGING_EINVAL,"PTE count field overflow rejected before array read");
	check(bc250_sdma_paging_write_ptes(&g_adev,buf,32,0x2000,NULL,3,&written)==BC250_SDMA_PAGING_EINVAL,"PTE NULL values rejected");
	check(all_pattern(buf,32,0xCCCCCCCCu),"invalid PTE arguments write nothing");
}

/* Synthetic register identifiers for replay, not hardware addresses. */
enum { TEST_REQ_ID = 0x40, TEST_ACK_ID = 0x44 };
static unsigned int test_request_calls;
static unsigned int test_expected_vmid;
static u32 test_invalidate_req(unsigned int vmid, u32 type)
{
	check(vmid==test_expected_vmid && type==0,"invalidate uses selected VMID and flush type0");
	test_request_calls++;
	return 0x13570000u | (1u << vmid); /* opaque encoder witness, not a hardware request constant */
}
static void case_gart_invalidate(void)
{
	static const struct amdgpu_vmhub_funcs funcs = {NULL,test_invalidate_req};
	struct amdgpu_vmhub *hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
	u32 buf[32];unsigned int cap,written;
	const u32 write=SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE)|SDMA_PKT_SRBM_WRITE_HEADER_BYTE_EN(0xfu);
	const u32 poll=SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM)|SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3);
	const u32 retry=SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff)|SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(10);
	hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;hub->vmhub_funcs=&funcs;
	memset(buf,0xCC,sizeof(buf));test_request_calls=0;
	check(bc250_sdma_paging_invalidate_gart(&g_adev,buf,16,&written)==BC250_SDMA_PAGING_OK && written==15,"GART flush emits15DW within16DW reservation");
	check(test_request_calls==1 && buf[0]==write && buf[1]==TEST_REQ_ID && buf[2]==0x13570001u,"flush writes derived request before polls");
	check(buf[3]==poll && buf[4]==TEST_REQ_ID*4 && buf[5]==0 && buf[6]==0 && buf[7]==0 && buf[8]==retry,"flush includes request read cycle before ACK");
	check(buf[9]==poll && buf[10]==TEST_ACK_ID*4 && buf[11]==0 && buf[12]==1 && buf[13]==1 && buf[14]==retry,"flush waits for VMID0 acknowledge");
	check(all_pattern(buf+15,17,0xCCCCCCCCu),"flush has no extra writes/root changes or trailing corruption");
	for(cap=0;cap<16;cap++) {
		memset(buf,0xCC,sizeof(buf));
		check(bc250_sdma_paging_invalidate_gart(&g_adev,buf,cap,&written)==BC250_SDMA_PAGING_INSUFFICIENT && written==16,"flush refuses undersized reservation");
		check(all_pattern(buf,32,0xCCCCCCCCu),"flush refusal writes nothing");
	}
    /* Each context must request and wait for its own ACK, including VMID15.
     * Insufficient capacity may not publish a request or any partial poll. */
    for (test_expected_vmid=0;test_expected_vmid<16;test_expected_vmid++) {
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_invalidate_vmid(&g_adev,buf,16,test_expected_vmid,&written)==BC250_SDMA_PAGING_OK && written==15,"VMID flush fits one reservation");
        check(buf[2]==(0x13570000u|(1u<<test_expected_vmid)) && buf[12]==(1u<<test_expected_vmid) && buf[13]==(1u<<test_expected_vmid),"request and ACK use same VMID");
        check(all_pattern(buf+15,17,0xCCCCCCCCu),"VMID flush preserves tail");
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_invalidate_vmid(&g_adev,buf,15,test_expected_vmid,&written)==BC250_SDMA_PAGING_INSUFFICIENT && written==16,"VMID flush requires aligned room");
        check(all_pattern(buf,32,0xCCCCCCCCu),"VMID insufficient writes nothing");
    }
    test_expected_vmid=0;test_request_calls=0;
    check(bc250_sdma_paging_invalidate_vmid(&g_adev,buf,32,16,&written)==BC250_SDMA_PAGING_EINVAL && !written,"out-of-range VMID refused");
    check(bc250_sdma_paging_invalidate_vmid(&g_adev,buf,32,~0u,&written)==BC250_SDMA_PAGING_EINVAL && !written,"large VMID refused before shift");
    check(test_request_calls==0 && all_pattern(buf,32,0xCCCCCCCCu),"invalid VMID never reaches encoder or output");
	hub->vmhub_funcs=NULL;
	check(bc250_sdma_paging_invalidate_gart(&g_adev,buf,32,&written)==BC250_SDMA_PAGING_EINVAL && !written,"uninitialized hub refuses flush construction");
	hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_ack=~0u;
	check(bc250_sdma_paging_invalidate_gart(&g_adev,buf,32,&written)==BC250_SDMA_PAGING_EINVAL,"register-byte address overflow refused");
	check(all_pattern(buf,32,0xCCCCCCCCu),"invalid hub descriptors write nothing");
	hub->vm_inv_eng0_ack=TEST_ACK_ID;hub->vm_inv_eng0_req=SDMA_PKT_SRBM_WRITE_ADDR_addr_mask+1u;
	check(bc250_sdma_paging_invalidate_gart(&g_adev,buf,32,&written)==BC250_SDMA_PAGING_EINVAL,"request outside SRBM register field refused");
	memset(hub,0,sizeof(*hub));
}

static void case_mapped_copy(void)
{
	static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
	struct amdgpu_vmhub *hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
	const u64 ptes[2]={0x123450067ULL,0xABCDF0067ULL};
	struct bc250_sdma_paging_mapping map={0x100002000ULL,ptes,2,0x200003000ULL,101,0x300000123ULL,0x300001231ULL,128};
	u32 buf[128];unsigned int written,cap,i;
	const unsigned int barriers[3]={8,40,58},invalidates[2]={18,68};
	hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
	memset(buf,0xCC,sizeof(buf));
	check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,96,&map,&written)==BC250_SDMA_PAGING_OK && written==83,"whole two-page mapped transfer emits83DW within96DW reservation");
	check(buf[0]==SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) && buf[1]==0x2000 && buf[2]==1 && buf[3]==3,"transaction starts with two PTE writes");
	check(buf[4]==0x23450067 && buf[5]==1 && buf[6]==0xBCDF0067 && buf[7]==0xA,"transaction preserves scattered mapping values");
	for(i=0;i<3;i++) {
		unsigned int at=barriers[i];
		check(buf[at]==(SDMA_PKT_HEADER_OP(SDMA_OP_FENCE)|SDMA_PKT_FENCE_HEADER_MTYPE(3)) && buf[at+1]==0x3000 && buf[at+2]==2 && buf[at+3]==101+i,"each phase writes its own scratch marker, without interrupt");
		check(buf[at+4]==(SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM)|SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3)|SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(1)) && buf[at+5]==0x3000 && buf[at+6]==2 && buf[at+7]==101+i && buf[at+8]==~0u && buf[at+9]==(SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff)|SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(4)),"phase barrier polls matching marker with AMD memory-poll fields");
	}
	for(i=0;i<2;i++) {
		unsigned int at=invalidates[i];
		check(buf[at]==(SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE)|SDMA_PKT_SRBM_WRITE_HEADER_BYTE_EN(0xfu)) && buf[at+1]==TEST_REQ_ID && buf[at+2]==0x13570001u,"map/unmap barriers precede TLB request");
		check(buf[at+4]==TEST_REQ_ID*4 && buf[at+7]==0 && buf[at+10]==TEST_ACK_ID*4 && buf[at+12]==1 && buf[at+13]==1,"both invalidations have reset-cycle and ACK polls");
	}
	check(buf[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && buf[34]==127 && buf[35]==0 && buf[36]==0x123 && buf[37]==3 && buf[38]==0x1231 && buf[39]==3,"copy uses exact source/destination/count between mapping and cleanup barriers");
	check(buf[50]==SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) && buf[51]==0x2000 && buf[52]==1 && buf[53]==3 && all_zero(buf+54,4),"cleanup clears same slots only after copy fence/poll");
	check(all_pattern(buf+83,45,0xCCCCCCCCu),"whole transaction leaves trailing memory untouched");
	for(cap=0;cap<96;cap++) {
		memset(buf,0xCC,sizeof(buf));
		check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,cap,&map,&written)==BC250_SDMA_PAGING_INSUFFICIENT && written==96,"transaction budget includes map/unmap/barriers and align");
		check(all_pattern(buf,128,0xCCCCCCCCu),"no partial transaction on insufficient capacity");
	}
	map.first_sequence=~0u-1u;
	check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL && !written,"marker wrap rejected before publication");
	map.first_sequence=0;
	check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL,"zero initial marker rejected");
	map.first_sequence=101;map.scratch_mc++;
	check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL,"unaligned scratch marker refused");
	check(all_pattern(buf,128,0xCCCCCCCCu),"invalid transaction writes nothing");
	memset(hub,0,sizeof(*hub));
}

static void case_ordered_ptes(void)
{
    u64 ptes[512];u32 buf[1056];unsigned int i,count,written,need,at,cap;
    const u64 table=0xF401000000ULL,scratch=0x300400028ULL;
    for(i=0;i<512;i++)ptes[i]=0x1234567800000000ULL+((u64)i<<12)+0x67;
    for(count=1;count<=512;count++) {
        memset(buf,0xCC,sizeof(buf));
        need=(14u+2u*count+15u)&~15u;
        check(bc250_sdma_paging_update_ptes(&g_adev,buf,need,table,ptes,count,scratch,29,&written)==BC250_SDMA_PAGING_OK && written==14+2*count,"ordered PTE update fits exact aligned reservation");
        check(buf[0]==SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) && buf[1]==(u32)table && buf[2]==(u32)(table>>32) && buf[3]==2*count-1,"ordered PTE write preserves address/count");
        for(i=0;i<count;i++)if(buf[4+2*i]!=(u32)ptes[i] || buf[5+2*i]!=(u32)(ptes[i]>>32))break;
        check(i==count,"ordered write preserves every64-bit PTE");
        at=4+2*count;
        check(buf[at]==(SDMA_PKT_HEADER_OP(SDMA_OP_FENCE)|SDMA_PKT_FENCE_HEADER_MTYPE(3)) && buf[at+1]==(u32)scratch && buf[at+2]==(u32)(scratch>>32) && buf[at+3]==29,"PTE data precedes internal noninterrupting fence");
        check(buf[at+4]==(SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM)|SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3)|SDMA_PKT_POLL_REGMEM_HEADER_MEM_POLL(1)) && buf[at+5]==(u32)scratch && buf[at+6]==(u32)(scratch>>32) && buf[at+7]==29 && buf[at+8]==~0u,"barrier polls matching PTE marker");
        check(all_pattern(buf+written,1056-written,0xCCCCCCCCu),"ordered PTE update leaves tail untouched");
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_update_ptes(&g_adev,buf,need-1,table,ptes,count,scratch,29,&written)==BC250_SDMA_PAGING_INSUFFICIENT && written==need && all_pattern(buf,1056,0xCCCCCCCCu),"one DWORD short cannot expose partial PTE write");
    }
    for(cap=0;cap<32;cap++) {
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_update_ptes(&g_adev,buf,cap,table,ptes,2,scratch,29,&written)==BC250_SDMA_PAGING_INSUFFICIENT && all_pattern(buf,1056,0xCCCCCCCCu),"two-PTE transaction refuses every undersized capacity");
    }
    check(bc250_sdma_paging_update_ptes(&g_adev,buf,1056,~7ULL,ptes,2,scratch,1,&written)==BC250_SDMA_PAGING_EINVAL && !written,"PTE destination overflow refused");
    check(bc250_sdma_paging_update_ptes(&g_adev,buf,1056,table,ptes,2,scratch+1,1,&written)==BC250_SDMA_PAGING_EINVAL,"unaligned scratch refused");
    check(bc250_sdma_paging_update_ptes(&g_adev,buf,1056,table,ptes,2,0,1,&written)==BC250_SDMA_PAGING_EINVAL,"absent scratch refused");
    check(bc250_sdma_paging_update_ptes(&g_adev,buf,1056,table,ptes,2,scratch,0,&written)==BC250_SDMA_PAGING_EINVAL,"zero marker refused");
    check(bc250_sdma_paging_update_ptes(&g_adev,buf,1056,table,NULL,2,scratch,1,&written)==BC250_SDMA_PAGING_EINVAL,"absent PTE array refused");
    check(all_pattern(buf,1056,0xCCCCCCCCu),"invalid ordered updates leave buffer intact");
}

static void case_staged_pte_copy(void)
{
    u32 buf[64];unsigned int written,cap,src,dst,choice,i,bytes;
    const unsigned int offsets[]={0,1,63,64,255,480,511};
    const unsigned int counts[]={1,2,31,32,128,256,512};
    const u64 base=0x100000000ull,stage=0x100002000ull,marker=0x100003000ull;
    static unsigned char memory[16384],expected[16384];
    for(src=0;src<7;src++)for(dst=0;dst<7;dst++)for(choice=0;choice<7;choice++) {
        unsigned int count=counts[choice];u64 a=base+offsets[src]*8u,b=base+offsets[dst]*8u;
        if(count>512-offsets[src] || count>512-offsets[dst])continue;
        bytes=count*8u;memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_copy_ptes(&g_adev,buf,48,a,b,count,stage,marker,9,&written)==BC250_SDMA_PAGING_OK && written==34,"staged PTE copy reserves both copies and barriers");
        check(buf[0]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && buf[1]==bytes-1 && buf[2]==0 && buf[3]==(u32)a && buf[4]==(u32)(a>>32) && buf[5]==(u32)stage && buf[6]==(u32)(stage>>32),"first copy snapshots source into staging");
        check(buf[17]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && buf[18]==bytes-1 && buf[19]==0 && buf[20]==(u32)stage && buf[21]==(u32)(stage>>32) && buf[22]==(u32)b && buf[23]==(u32)(b>>32),"second copy writes staged bytes to destination");
        check(buf[7]==(SDMA_PKT_HEADER_OP(SDMA_OP_FENCE)|SDMA_PKT_FENCE_HEADER_MTYPE(3)) && buf[8]==(u32)marker && buf[9]==(u32)(marker>>32) && buf[10]==9 && buf[14]==9 && buf[15]==0xffffffffu,"first phase noninterrupting marker and matching poll");
        check(buf[24]==(SDMA_PKT_HEADER_OP(SDMA_OP_FENCE)|SDMA_PKT_FENCE_HEADER_MTYPE(3)) && buf[25]==(u32)marker && buf[26]==(u32)(marker>>32) && buf[27]==10 && buf[31]==10 && buf[32]==0xffffffffu,"second phase uses distinct fresh marker");
        check(buf[11]==buf[28] && (buf[11]&0xffu)==SDMA_OP_POLL_REGMEM && buf[12]==(u32)marker && buf[13]==(u32)(marker>>32) && buf[29]==buf[12] && buf[30]==buf[13] && buf[16]==buf[33],"both barriers poll the marker slot");
        check(all_pattern(buf+34,30,0xCCCCCCCCu),"PTE transaction leaves tail guards unchanged");
        for(i=0;i<sizeof(memory);i++)memory[i]=(unsigned char)(i*17u+i/256u);
        memcpy(expected,memory,sizeof(memory));
        memmove(expected+(size_t)(b-base),expected+(size_t)(a-base),bytes);
        // Interpret only the two validated copy payloads; barrier timing is not
        // modeled. Each actual memcpy is disjoint; oracle uses one memmove.
        memcpy(memory+buf[5],memory+buf[3],buf[1]+1u);
        memcpy(memory+buf[22],memory+buf[20],buf[18]+1u);
        check(memcmp(memory,expected,4096)==0,"encoded staged copies match snapshot oracle for overlap and self-copy");
    }
    for(cap=0;cap<48;cap++) {
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_copy_ptes(&g_adev,buf,cap,base,base+8,32,stage,marker,9,&written)==BC250_SDMA_PAGING_INSUFFICIENT && written==48 && all_pattern(buf,64,0xCCCCCCCCu),"every undersized capacity refuses whole staged copy");
    }
    memset(buf,0xCC,sizeof(buf));
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,base+8,0,stage,marker,9,&written)==BC250_SDMA_PAGING_EINVAL && !written,"empty PTE copy rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,base+8,513,stage,marker,9,&written)==BC250_SDMA_PAGING_EINVAL,"more than one table rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base+1,base+8,1,stage,marker,9,&written)==BC250_SDMA_PAGING_EINVAL,"unaligned PTE address rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,base+8,1,stage+8,marker,9,&written)==BC250_SDMA_PAGING_EINVAL,"unaligned staging page rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,stage+4000,base+8,1,stage,marker,9,&written)==BC250_SDMA_PAGING_EINVAL,"any staging-page source alias rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,stage+4000,1,stage,marker,9,&written)==BC250_SDMA_PAGING_EINVAL,"any staging-page destination alias rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,base+8,2,stage,base+12,9,&written)==BC250_SDMA_PAGING_EINVAL,"marker destination alias rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,base+8,1,stage,stage+4092,9,&written)==BC250_SDMA_PAGING_EINVAL,"marker staging alias rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,~0ull-7,base,2,stage,marker,9,&written)==BC250_SDMA_PAGING_EINVAL,"source end overflow rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,~0ull-7,2,stage,marker,9,&written)==BC250_SDMA_PAGING_EINVAL,"destination end overflow rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,base+8,1,stage,marker,0,&written)==BC250_SDMA_PAGING_EINVAL,"zero marker rejected");
    check(bc250_sdma_paging_copy_ptes(&g_adev,buf,64,base,base+8,1,stage,marker,~0u,&written)==BC250_SDMA_PAGING_EINVAL,"marker wrap rejected");
    check(all_pattern(buf,64,0xCCCCCCCCu),"invalid staged-copy inputs never modify output");
}

static void case_staged_byte_copy(void)
{
    const u64 base=0x100000000ull,stage=0x100002000ull,marker=0x100003000ull;
    const unsigned offsets[]={0,1,3,7,8,31,63,4093,4095};
    const unsigned sizes[]={1,2,3,7,8,17,63,511,4095,4096};
    static unsigned char memory[16384],expected[16384];
    u32 buf[64];unsigned a,b,n,i,bytes,written,cap;
    for(a=0;a<9;a++)for(b=0;b<9;b++)for(n=0;n<10;n++) {
        u64 src=base+offsets[a],dst=base+offsets[b];bytes=sizes[n];
        if(bytes>4096-offsets[a] || bytes>4096-offsets[b])continue;
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_copy_bytes(&g_adev,buf,48,src,dst,bytes,stage,marker,21,&written)==BC250_SDMA_PAGING_OK && written==34,"staged arbitrary byte range accepted atomically");
        check(buf[1]+1==bytes && buf[18]+1==bytes && buf[3]==offsets[a] && buf[22]==offsets[b] &&
              buf[5]==8192 && buf[20]==8192 && buf[4]==1 && buf[23]==1,"staged byte packet source stage destination and size match request");
        check(buf[10]==21 && buf[14]==21 && buf[27]==22 && buf[31]==22,"staged byte barriers use two matching distinct markers");
        check(all_pattern(buf+34,30,0xCCCCCCCCu),"staged byte packet tail guard");
        for(i=0;i<sizeof(memory);i++)memory[i]=(unsigned char)(i*37u+i/123u);
        memcpy(expected,memory,sizeof(memory));memmove(expected+offsets[b],expected+offsets[a],bytes);
        memcpy(memory+buf[5],memory+buf[3],buf[1]+1);
        memcpy(memory+buf[22],memory+buf[20],buf[18]+1);
        check(memcmp(memory,expected,8192)==0,"decoded staged bytes match independent overlap snapshot");
    }
    for(cap=0;cap<48;cap++) {
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_copy_bytes(&g_adev,buf,cap,base+1,base+3,17,stage,marker,21,&written)==BC250_SDMA_PAGING_INSUFFICIENT && written==48 && all_pattern(buf,64,0xCCCCCCCCu),"all short capacities refuse before partial staged-byte emission");
    }
    check(bc250_sdma_paging_copy_bytes(&g_adev,buf,64,base,base,0,stage,marker,21,&written)==BC250_SDMA_PAGING_EINVAL && !written,"zero byte staging refuses");
    check(bc250_sdma_paging_copy_bytes(&g_adev,buf,64,base,base,4097,stage,marker,21,&written)==BC250_SDMA_PAGING_EINVAL,"staging capacity overrun refuses");
    check(bc250_sdma_paging_copy_bytes(&g_adev,buf,64,~0ull,base,2,stage,marker,21,&written)==BC250_SDMA_PAGING_EINVAL,"staged byte source wrap refuses");
    check(bc250_sdma_paging_copy_bytes(&g_adev,buf,64,base,~0ull,2,stage,marker,21,&written)==BC250_SDMA_PAGING_EINVAL,"staged byte destination wrap refuses");
    check(bc250_sdma_paging_copy_bytes(&g_adev,buf,64,stage+4095,base,1,stage,marker,21,&written)==BC250_SDMA_PAGING_EINVAL,"single byte source alias of reserved stage refuses");
    check(bc250_sdma_paging_copy_bytes(&g_adev,buf,64,base,marker+3,1,stage,marker,21,&written)==BC250_SDMA_PAGING_EINVAL,"single byte destination alias of marker refuses");
}

static void case_mapped_staging(void)
{
    static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
    struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
    struct bc250_sdma_paging_mapping map={0};u64 ptes[2]={0x123450067ull,0x123450067ull};
    u32 buf[128];unsigned written,cap,i;unsigned char memory[16384],expected[16384];
    hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
    map.table_mc=0x600000000ull;map.ptes=ptes;map.page_count=2;map.scratch_mc=0x500000000ull;
    map.first_sequence=101;map.src_mc=0x300000001ull;map.dst_mc=0x300001003ull;
    map.bytes=127;map.staging_mc=0x400000000ull;
    memset(buf,0xCC,sizeof(buf));
    check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,112,&map,&written)==BC250_SDMA_PAGING_OK && written==100,"mapped staged copy reserves complete100-DWORD transaction");
    check(buf[36]==1 && buf[37]==3 && buf[38]==0 && buf[39]==4 && buf[53]==0 && buf[54]==4 && buf[55]==4099 && buf[56]==3,
          "mapped stage captures two disjoint COPY payloads through retained GART slots");
    check(buf[11]==101 && buf[43]==102 && buf[60]==103 && buf[78]==104 && buf[47]==102 && buf[64]==103 && buf[82]==104,
          "map snapshot destination and cleanup phases have four fresh matching markers");
    check(buf[71]==0 && buf[72]==0 && buf[73]==0 && buf[74]==0 && all_pattern(buf+100,28,0xCCCCCCCCu),
          "mapped PTE cleanup occurs after second copy and leaves tail guard");
    for(i=0;i<sizeof(memory);i++)memory[i]=(unsigned char)(i*31+i/257);
    memcpy(expected,memory,sizeof(memory));memmove(expected+3,expected+1,127);
    // Both PTEs name the same physical page; translate slot offsets independently.
    memcpy(memory+8192,memory+buf[36],buf[34]+1);
    memcpy(memory+buf[55]-4096,memory+8192,buf[51]+1);
    check(memcmp(memory,expected,8192)==0,"mapped alias staged bytes match physical memmove oracle");
    for(cap=0;cap<112;cap++) {
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,cap,&map,&written)==BC250_SDMA_PAGING_INSUFFICIENT &&
              written==112 && all_pattern(buf,128,0xCCCCCCCCu),"short mapped-stage capacity refuses before PTE writes");
    }
    map.staging_mc=map.table_mc;
    check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL && !written,"stage cannot alias GART table storage");
    map.staging_mc=map.scratch_mc;
    check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL,"stage cannot alias synchronization marker");
    map.staging_mc=0x300000000ull;
    check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL,"stage cannot alias source window range");
    map.staging_mc=0x400000000ull;map.first_sequence=~0u-2;
    check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL,"four-phase marker wrap refuses");
    map.first_sequence=101;map.bytes=4097;
    check(bc250_sdma_paging_mapped_transfer(&g_adev,buf,128,&map,&written)==BC250_SDMA_PAGING_EINVAL,"mapped stage refuses oversized snapshot");
    memset(hub,0,sizeof(*hub));
}

static void case_aperture_packets(void)
{
    static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
    struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
    static const unsigned counts[]={1,2,7,16,31,128,480,497,512};
    u32 buf[1088],reference[1088],flush[16];u64 ptes[512];
    const u64 table=0x400020010ull,scratch=0x500000000ull,limit=0x1000000000000ull;
    unsigned i,j,count,need,written,updateWords,flushWords,cap;
    hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
    test_expected_vmid=0;
    for(i=0;i<512;i++)ptes[i]=(0x12345000ull+(u64)i*8192)|0x73;
    for(i=0;i<sizeof(counts)/sizeof(counts[0]);i++) {
        count=counts[i];need=(29+2*count+15)&~15u;updateWords=flushWords=0;
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_set_aperture(&g_adev,buf,need,table,ptes,count,scratch,91,&written)==BC250_SDMA_PAGING_OK &&
              written==29+2*count,"aperture writes and invalidation fit complete aligned reservation");
        check(bc250_sdma_paging_update_ptes(&g_adev,reference,1088,table,ptes,count,scratch,91,&updateWords)==BC250_SDMA_PAGING_OK &&
              bc250_sdma_paging_invalidate_gart(&g_adev,flush,16,&flushWords)==BC250_SDMA_PAGING_OK,
              "independent update and GART flush constructors provide positive controls");
        check(written==updateWords+flushWords && memcmp(buf,reference,updateWords*4)==0 &&
              memcmp(buf+updateWords,flush,flushWords*4)==0,
              "aperture transaction is exact ordered update then VMID0 flush");
        for(j=0;j<count;j++)
            check((((u64)buf[5+j*2]<<32)|buf[4+j*2])==ptes[j],"aperture payload preserves each scattered PTE");
        check(all_pattern(buf+written,1088-written,0xCCCCCCCCu),"aperture transaction preserves output tail");
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_set_aperture(&g_adev,buf,need-1,table,ptes,count,scratch,91,&written)==BC250_SDMA_PAGING_INSUFFICIENT &&
              written==need && all_pattern(buf,1088,0xCCCCCCCCu),"aperture short reservation exposes no partial update");
    }
    for(cap=0;cap<48;cap++) {
        memset(buf,0xCC,sizeof(buf));
        check(bc250_sdma_paging_set_aperture(&g_adev,buf,cap,table,ptes,2,scratch,91,&written)==BC250_SDMA_PAGING_INSUFFICIENT &&
              written==48 && all_pattern(buf,1088,0xCCCCCCCCu),"every short two-page aperture buffer stays untouched");
    }
    for(i=0;i<4;i++)ptes[i]=0x98765073ull;
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,48,table,ptes,4,scratch,~0u,&written)==BC250_SDMA_PAGING_OK &&
          buf[4]==0x98765073u && buf[6]==buf[4] && buf[8]==buf[4] && buf[10]==buf[4],
          "unmap payload can repeat a nonzero dummy page with final nonzero marker");
    memset(buf,0xCC,sizeof(buf));
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,table,ptes,2,table,1,&written)==BC250_SDMA_PAGING_EINVAL && !written,
          "aperture marker cannot overwrite PTE payload");
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,table,ptes,2,table+12,1,&written)==BC250_SDMA_PAGING_EINVAL,
          "aperture marker cannot overlap final PTE half");
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,limit-8,ptes,2,scratch,1,&written)==BC250_SDMA_PAGING_EINVAL,
          "aperture PTE address cannot cross48bit end");
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,table,ptes,2,limit,1,&written)==BC250_SDMA_PAGING_EINVAL,
          "aperture marker must be representable");
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,table+1,ptes,2,scratch,1,&written)==BC250_SDMA_PAGING_EINVAL,
          "aperture table alignment checked");
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,table,ptes,2,scratch,0,&written)==BC250_SDMA_PAGING_EINVAL,
          "aperture marker must be nonzero");
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,table,NULL,2,scratch,1,&written)==BC250_SDMA_PAGING_EINVAL,
          "aperture unmap cannot accidentally substitute zero PTEs for dummy page");
    hub->vmhub_funcs=NULL;
    check(bc250_sdma_paging_set_aperture(&g_adev,buf,1088,table,ptes,2,scratch,1,&written)==BC250_SDMA_PAGING_EINVAL &&
          !written && all_pattern(buf,1088,0xCCCCCCCCu),"invalid aperture transactions write nothing including absent flush context");
    memset(hub,0,sizeof(*hub));
}

#define InterlockedAdd64(p,v) (*(p)+=(v))
typedef long long LONG64;

#include "paging_window.h"
#include "paging_intervals.h"
#include "paging_permutation.h"
#define BC250_GFX_TAG 0
#include "paging_aperture_state.h"
#define DXGK_OPERATION_MAP_APERTURE_SEGMENT 5
#define DXGK_OPERATION_UNMAP_APERTURE_SEGMENT 6
#include "paging_private.h"
#include "paging_pt_shadow.h"
#define BC250_VIDMM_SHADOW_TAG 0
#define BC250_VIDMM_PAGING_VA_BYTES (1ull<<30)
#include "paging_stream.h"
#include "paging_mc.h"
#include "bc250_gart.h"
typedef unsigned long long ULONGLONG;
typedef unsigned long ULONG;
typedef int BOOLEAN;
typedef unsigned int UINT;
#define TRUE 1
#define FALSE 0
#define PAGE_SIZE 4096
#define BC250_PAGING_MARKER_SLOT 5
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef enum {BC250PagingSupported,BC250PagingNotReady,BC250PagingNoTranslation,BC250PagingSystemMemory} BC250_WDDM_PAGING_UNSUPPORTED;
typedef struct {struct {long long QuadPart;} VramPhysical;ULONGLONG VramMcBase,VramLength;void*Gfx;int GfxPagingLock;BOOLEAN FullWddm,VramEnabled,VramWriteEnabled;struct {ULONG Pitch,Height;} Post;int GartLock;PAGING_APERTURE WddmAperture;} BC250_DEVICE;
typedef struct {BOOLEAN PagingWindowReady;PAGING_WINDOW PagingWindow;struct amdgpu_device*PagingDevicePtr;BOOLEAN PagingReady;struct amdgpu_ring*PagingRing;BOOLEAN PagingCpuBootstrap;struct bc250_mem PagingCopyStaging;} BC250_GFX;
static ULONGLONG translated;static int isSystem,translationOk=1,fragmented;
static int use_retained_walk,translation_by_offset;
static BOOLEAN VidMmTranslateRetainedPaging(ULONGLONG,ULONGLONG,ULONGLONG*,BOOLEAN*);
static int VidMmTranslatePaging(ULONGLONG r,ULONGLONG v,ULONGLONG*p,BOOLEAN*s){if(use_retained_walk)return VidMmTranslateRetainedPaging(r,v,p,s);(void)r;(void)v;*p=fragmented ? 0x100000ull+((v/4096)%17)*8192+(v&4095) : translated+(translation_by_offset?(v&4095):0);*s=isSystem;return translationOk;}

typedef long NTSTATUS;
typedef void* PVOID;
#define STATUS_NOT_SUPPORTED (-6)
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_DEVICE_NOT_READY (-2)
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER (-3)
static int gart_capture_lock,gart_capture_calls,gart_capture_unlocked;
static NTSTATUS gart_capture_status=STATUS_SUCCESS;
static void ExAcquireFastMutex(int* lock){(void)lock;gart_capture_lock++;}
static void ExReleaseFastMutex(int* lock){(void)lock;gart_capture_lock--;}
static NTSTATUS GartDevice(BC250_DEVICE* d,struct amdgpu_device** a,BOOLEAN* enabled)
{
 (void)d;gart_capture_calls++;
 if(gart_capture_lock!=1)gart_capture_unlocked++;
 *a=gart_capture_status==STATUS_SUCCESS?&g_adev:NULL;*enabled=FALSE;
 return gart_capture_status;
}
#define BC250_GFX_PAGING_BUFFER_BYTES 65536u
static int flush_lock_depth,flush_region_depth,cpu_lock_depth;
static void ExAcquirePushLockExclusive(int*p){(void)p;cpu_lock_depth++;}
static void ExReleasePushLockExclusive(int*p){(void)p;cpu_lock_depth--;}
static void KeEnterCriticalRegion(void){flush_region_depth++;}
static void KeLeaveCriticalRegion(void){flush_region_depth--;}
static void ExAcquirePushLockShared(int*p){(void)p;flush_lock_depth++;}
static void ExReleasePushLockShared(int*p){(void)p;flush_lock_depth--;}

#include "bc250_pte.h"
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define BC250_VIDMM_LEVELS 4u
#define BC250_VIDMM_PTES 512u
#define DXGK_PAGETABLEUPDATE_GPU_PHYSICAL 1
#define DXGK_PAGETABLEUPDATE_CPU_VIRTUAL 2
/* Field-level host model, not a WDK layout test; the full KMD build checks ABI. */
typedef struct {ULONG SegmentId;ULONGLONG SegmentOffset;} D3DGPU_PHYSICAL_ADDRESS;
typedef struct {ULONGLONG Flags,PageAddress;} DXGK_PTE;
typedef struct {
 UINT UpdateMode,PageTableLevel,NumPageTableEntries,StartIndex;
 struct {int Use64KBPages,Repeat;} Flags;
 union {D3DGPU_PHYSICAL_ADDRESS GpuPhysical;void*CpuVirtual;} PageTableAddress;
 const DXGK_PTE*pPageTableEntries;
} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
typedef struct {PAGING_APERTURE_STATE Aperture;PAGING_PT_SHADOW Shadow;int CpuUpdateLock;ULONGLONG CpuEntries[512];BOOLEAN Ready,Write;ULONGLONG SegmentPhysical,SegmentLength;unsigned char*SegmentMapping;struct bc250_pte_context Pte;long Calls[4],CpuCalls,GpuCalls,BadCalls,Refused;long long Entries[4],Valid[4],Written,EncodedCoherentSystem,EncodedUncachedSystem,EncodedCoherencyMismatch;} BC250_VIDMM;
static BC250_VIDMM g_VidMm;
static enum bc250_pte_kind VidMmKind(UINT level){return level ? BC250_PTE_DIRECTORY : BC250_PTE_LEAF;}
static void KeMemoryBarrier(void){}

#include <excpt.h>
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_INSUFFICIENT_RESOURCES (-4)
#define BC250_VIDMM_LOG_CALLS 4
#define POOL_FLAG_PAGED 1
#define PAGE_READWRITE 2
#define PAGE_NOCACHE 4
typedef size_t SIZE_T;
typedef size_t ULONG_PTR;
typedef long long LONGLONG;
typedef struct {long long QuadPart;} PHYSICAL_ADDRESS;
static long TestIncrement(long*p){return ++*p;}
static long long TestIncrement64(long long*p){return ++*p;}
#define InterlockedIncrement TestIncrement
#define InterlockedIncrement64 TestIncrement64
static int cpu_map_fail,cpu_map_live,cpu_map_calls;
static ULONGLONG last_map_physical;static SIZE_T last_map_bytes;
__declspec(align(4096)) static u64 cpu_storage[4096];
#define cpu_table (cpu_storage+512)
static void*MmMapIoSpaceEx(PHYSICAL_ADDRESS p,SIZE_T bytes,unsigned protect){last_map_physical=(ULONGLONG)p.QuadPart;last_map_bytes=bytes;(void)protect;cpu_map_calls++;if(cpu_map_fail)return NULL;cpu_map_live++;return cpu_storage;}
static void MmUnmapIoSpace(void*p,SIZE_T bytes){(void)p;(void)bytes;cpu_map_live--;}
static void GuardLog(const char*format,...){(void)format;}

typedef unsigned char* PUCHAR;
#define MAXULONGLONG (~0ull)
#define PAGE_SHIFT 12
#define PASSIVE_LEVEL 0
static int KeGetCurrentIrql(void){return PASSIVE_LEVEL;}
static void ExInitializePushLock(int*p){*p=0;}
static unsigned cpu_write_setting=1;
static unsigned GuardReadSetting(const wchar_t*name,unsigned def){(void)name;(void)def;return cpu_write_setting;}
static ULONGLONG VidMmSystemLimit(void){return 0;}

static int shadow_pool_fail,shadow_pool_live;
static void*ExAllocatePool2(unsigned flags,SIZE_T bytes,unsigned tag){void*p;(void)flags;(void)tag;if(shadow_pool_fail)return NULL;p=calloc(1,bytes);if(p)shadow_pool_live++;return p;}
static void ExFreePoolWithTag(void*p,unsigned tag){(void)tag;free(p);shadow_pool_live--;}
static ULONGLONG override_cpu_physical;
static PHYSICAL_ADDRESS MmGetPhysicalAddress(void*p){PHYSICAL_ADDRESS a;a.QuadPart=(long long)(override_cpu_physical ? override_cpu_physical : g_VidMm.SegmentPhysical+(ULONGLONG)((unsigned char*)p-(unsigned char*)cpu_storage));return a;}

typedef struct {
 UINT NumPageTableEntries;ULONGLONG SrcPageTableAddress,DstPageTableAddress;
 UINT SrcStartPteIndex,DstStartPteIndex;
} DXGK_BUILDPAGINGBUFFER_COPY_RANGE;

// Match the anonymous struct/union syntax of the real WDK Transfer fields.
#pragma warning(push)
#pragma warning(disable:4201)
typedef union {
 struct {UINT Swizzle:1;UINT Unswizzle:1;UINT AllocationIsIdle:1;UINT TransferStart:1;UINT TransferEnd:1;UINT Reserved:27;};
 UINT Value;
} DXGK_TRANSFERFLAGS;
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
typedef union {struct {UINT CacheCoherent:1;UINT Reserved:31;};UINT Value;} DXGK_MAPAPERTUREFLAGS;
typedef struct {
 UINT Operation;
 void*pDmaBuffer;void*pDmaBufferPrivateData;
 ULONG DmaSize,DmaBufferPrivateDataSize,DmaBufferWriteOffset,MultipassOffset;
 struct {UINT NumRanges;DXGK_BUILDPAGINGBUFFER_COPY_RANGE*pRanges;} CopyPageTableEntries;
 struct {
  SIZE_T TransferSize;UINT TransferOffset,MdlOffset;DXGK_TRANSFERFLAGS Flags;
  struct {UINT SegmentId;union {PHYSICAL_ADDRESS SegmentAddress;void*pMdl;};} Source,Destination;
 } Transfer;
 struct {SIZE_T FillSize;UINT FillPattern;struct {UINT SegmentId;PHYSICAL_ADDRESS SegmentAddress;} Destination;} Fill;
 struct {UINT SegmentId;SIZE_T OffsetInPages,NumberOfPages;void* pMdl;DXGK_MAPAPERTUREFLAGS Flags;ULONG MdlOffset;} MapApertureSegment;
 struct {UINT SegmentId;SIZE_T OffsetInPages,NumberOfPages;PHYSICAL_ADDRESS DummyPage;} UnmapApertureSegment;
 struct {ULONGLONG TransferSizeInBytes,SourceVirtualAddress,DestinationVirtualAddress;} TransferVirtual;
 struct {ULONGLONG FillSizeInBytes,DestinationVirtualAddress;ULONG FillPattern;} FillVirtual;
 ULONGLONG DmaBufferGpuVirtualAddress;
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE UpdatePageTable;
} DXGKARG_BUILDPAGINGBUFFER;
#pragma warning(pop)


#define BC250_VRAM_TOP_RESERVED 0x2000000ull
static int layout_fb_known=1;static ULONGLONG layout_fb_offset;
static BOOLEAN VramFramebufferOffset(const BC250_DEVICE*d,ULONGLONG*o){(void)d;*o=layout_fb_offset;return layout_fb_known;}

typedef unsigned char UCHAR;
#define STATUS_BUFFER_TOO_SMALL (-5)
#define BC250_WDDM_SEGMENT_APERTURE 2u
#define BC250_WDDM_APERTURE_BYTES 0x10000000ull
#define BC250_WDDM_PAGING_BUFFER_BYTES 0x10000ul
#define BC250_WDDM_LEVEL_COUNT 4u
#define BC250_WDDM_LEVEL_BITS 9u
#define BC250_WDDM_PAGE_TABLE_BYTES 4096u
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_SEGMENT_TABLES 3u
static BOOLEAN g_ApertureOffered;
static int WddmAnswersLogged(const BC250_DEVICE*d){(void)d;return 0;}
typedef struct {struct {UINT CpuVisible,LocalBudgetGroup,DirectFlip,Aperture,CacheCoherent,Value;} Flags;PHYSICAL_ADDRESS BaseAddress,CpuTranslatedAddress;SIZE_T Size,CommitLimit;} DXGK_SEGMENTDESCRIPTOR4;
typedef struct {UINT NbSegment,SegmentDescriptorStride;void*pSegmentDescriptor;UINT PagingBufferSegmentId,PagingBufferSize,PagingBufferPrivateDataSize;} DXGK_QUERYSEGMENTOUT4;
typedef struct {UINT InputDataSize,OutputDataSize;void*pInputData;void*pOutputData;} DXGKARG_QUERYADAPTERINFO;
typedef struct {UINT LevelIndex;} DXGK_QUERYPAGETABLELEVELDESCIN;
typedef struct {UINT PageTableIndexBitCount,PageTableSizeInBytes,PageTableAlignmentInBytes,PageTableSegmentId,PagingProcessPageTableSegmentId;} DXGK_PAGE_TABLE_LEVEL_DESC;

#define BC250_IB_PROBE_DWORDS 240u
#define MM_COPY_MEMORY_PHYSICAL 1u
typedef union {PHYSICAL_ADDRESS PhysicalAddress;void*VirtualAddress;} MM_COPY_ADDRESS;
static int probe_copy_fail,probe_copy_short,probe_copy_calls;static u64 probe_copy_address;static SIZE_T probe_copy_bytes;
static NTSTATUS MmCopyMemory(void*dst,MM_COPY_ADDRESS src,SIZE_T bytes,ULONG flags,SIZE_T*done){unsigned i;check(flags==MM_COPY_MEMORY_PHYSICAL && flush_lock_depth>0,"physical diagnostic copy under table lifetime lock");probe_copy_calls++;probe_copy_address=(u64)src.PhysicalAddress.QuadPart;probe_copy_bytes=bytes;*done=probe_copy_short?bytes-4:bytes;for(i=0;i<*done/4;i++)((ULONG*)dst)[i]=0xABC00000u+i;return probe_copy_fail?STATUS_INVALID_PARAMETER:STATUS_SUCCESS;}

// MDL/PFN layout model: the full WDK build checks real platform types/macros.
typedef ULONGLONG PFN_NUMBER;
typedef struct {ULONG ByteCount,ByteOffset;} MDL,*PMDL;
#define MmGetMdlByteOffset(m) ((m)->ByteOffset)
#define MmGetMdlByteCount(m) ((m)->ByteCount)
#define MmGetMdlPfnArray(m) ((PFN_NUMBER*)((m)+1))

// Mdl != NULL selects OS PFNs; otherwise Address is already an MC address.
// Length bounds bytes from this endpoint's start; FirstPage applies only to MDLs.
typedef struct _BC250_PAGING_ENDPOINT {
    PMDL Mdl;
    ULONGLONG Address,Length;
    ULONG FirstPage;
    BOOLEAN Aperture;
} BC250_PAGING_ENDPOINT;
BOOLEAN GfxPagingEndpointValid(const BC250_DEVICE*,const BC250_PAGING_ENDPOINT*,ULONGLONG);

typedef struct _BC250_PAGING_APERTURE_OP {
    PMDL Mdl;
    ULONGLONG FirstPage,PageCount,DummyPhysical;
    ULONG MdlOffset;
    BOOLEAN Unmap,CacheCoherent;
} BC250_PAGING_APERTURE_OP;
typedef struct _BC250_PAGING_COPY_SLICE {
    ULONGLONG SourcePhysical,DestinationPhysical;
    ULONG Bytes;
    BOOLEAN SourceSystem,DestinationSystem;
} BC250_PAGING_COPY_SLICE;

BOOLEAN VidMmResolveAperture(ULONGLONG Mc, ULONG Bytes, ULONGLONG* Physical);
BOOLEAN VidMmApertureRangeValid(ULONGLONG Mc, ULONGLONG Bytes);
BOOLEAN VidMmRootPhysical(_In_ const D3DGPU_PHYSICAL_ADDRESS* Address, _Out_ ULONGLONG* Physical)
{
    const BC250_VIDMM* vm = &g_VidMm;

    *Physical = 0;
    // Plan-only mode (EnableGpuVa closed) has no root: tables nobody wrote are not tables to point a GPU at.
    if (!vm->Ready || !vm->Write || vm->SegmentLength < PAGE_SIZE || Address->SegmentId != (vm->Pte.table_size ? vm->Pte.table_segment : vm->Pte.vram_segment) ||
        (Address->SegmentOffset & (PAGE_SIZE - 1)) != 0 || Address->SegmentOffset > vm->SegmentLength - PAGE_SIZE)
        return FALSE;
    *Physical = vm->SegmentPhysical + Address->SegmentOffset;
    return TRUE;
}

BOOLEAN VidMmEncodePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                            ULONG Start, ULONG Count, _Out_ ULONGLONG* Physical,
                            _Out_writes_(Count) ULONGLONG* Entries)
{
    BC250_VIDMM* vm = &g_VidMm;
    UINT i;
    ULONGLONG table;
    if (Update == NULL || Entries == NULL || Physical == NULL ||
        Update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL ||
        Update->PageTableLevel >= BC250_VIDMM_LEVELS || Update->Flags.Use64KBPages ||
        Update->pPageTableEntries == NULL || Update->NumPageTableEntries == 0 ||
        Update->StartIndex >= BC250_VIDMM_PTES ||
        Update->NumPageTableEntries > BC250_VIDMM_PTES - Update->StartIndex ||
        Start > Update->NumPageTableEntries || Count == 0 || Count > Update->NumPageTableEntries - Start ||
        !VidMmRootPhysical(&Update->PageTableAddress.GpuPhysical,&table)) return FALSE;
    for (i = 0; i < Update->NumPageTableEntries; i++) {
        const DXGK_PTE* pte = Update->pPageTableEntries + (Update->Flags.Repeat ? 0 : i);
        u64 entry;
        if (bc250_pte_from_dxgk(&vm->Pte,VidMmKind(Update->PageTableLevel),
                               pte->Flags,pte->PageAddress,&entry) != 0) return FALSE;
        if (i >= Start && i - Start < Count) Entries[i - Start] = entry;
    }
    // Diagnostics count successful encoding attempts, including re-encoding at
    // logical publication. They do not imply GPU execution or page residency.
    if (Update->PageTableLevel==0) {
        LONG64 coherent=0,uncached=0,mismatch=0;
        for (i=0;i<Count;i++) {
            ULONGLONG entry=Entries[i];
            const DXGK_PTE* pte=Update->pPageTableEntries+(Update->Flags.Repeat?0:Start+i);
            if ((entry&(AMDGPU_PTE_VALID|AMDGPU_PTE_SYSTEM))!=(AMDGPU_PTE_VALID|AMDGPU_PTE_SYSTEM)) continue;
            if (pte->Flags&BC250_DXGK_PTE_CACHECOHERENT) coherent++; else uncached++;
            if (((pte->Flags&BC250_DXGK_PTE_CACHECOHERENT)!=0)!=((entry&AMDGPU_PTE_SNOOPED)!=0)) mismatch++;
        }
        if(coherent)InterlockedAdd64(&vm->EncodedCoherentSystem,coherent);
        if(uncached)InterlockedAdd64(&vm->EncodedUncachedSystem,uncached);
        if(mismatch)InterlockedAdd64(&vm->EncodedCoherencyMismatch,mismatch);
    }
    *Physical = table + ((ULONGLONG)Update->StartIndex + Start) * sizeof(ULONGLONG);
    return TRUE;
}

// Commit only a successfully constructed and capacity-accepted GPU batch.
// Source entries remain OS-owned and immutable throughout BuildPagingBuffer.
NTSTATUS VidMmCommitPagingUpdate(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                                 ULONG Start, ULONG Count)
{
    BC250_VIDMM* vm=&g_VidMm;
    ULONGLONG physical;
    int result;
    NTSTATUS status=STATUS_SUCCESS;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || vm->Shadow.Slots==NULL) status=STATUS_DEVICE_NOT_READY;
    else if (!VidMmEncodePageTable(Update,Start,Count,&physical,vm->CpuEntries)) status=STATUS_INVALID_PARAMETER;
    else {
        result=PagingPtShadowApply(&vm->Shadow,physical & ~(ULONGLONG)(PAGE_SIZE-1),
            (unsigned)((physical & (PAGE_SIZE-1))/sizeof(ULONGLONG)),Count,vm->CpuEntries,0);
        // Only pinned paging-process tables were registered during CPU initialization.
        // Ordinary process updates intentionally have no entry in this logical view.
        if (result!=PAGING_PT_OK && result!=PAGING_PT_MISSING) status=STATUS_INVALID_PARAMETER;
    }
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

// Physical entry identities come from a capacity-accepted copy range. Only
// registered pinned paging tables participate; no allocation or GPU write here.
NTSTATUS VidMmCommitPagingCopy(ULONGLONG Source, ULONGLONG Destination, ULONG Count)
{
    BC250_VIDMM* vm=&g_VidMm;
    NTSTATUS status=STATUS_SUCCESS;
    int result;
    if (((Source|Destination)&7ull)!=0) return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || vm->Shadow.Slots==NULL) status=STATUS_DEVICE_NOT_READY;
    else {
        result=PagingPtShadowCopy(&vm->Shadow,Source & ~(ULONGLONG)(PAGE_SIZE-1),
            (unsigned)((Source & (PAGE_SIZE-1))/8u),Destination & ~(ULONGLONG)(PAGE_SIZE-1),
            (unsigned)((Destination & (PAGE_SIZE-1))/8u),Count);
        // An ordinary application's destination has no construction-state mirror.
        // An unknown source copying into a registered table clears Known bits.
        if (result!=PAGING_PT_OK && result!=PAGING_PT_MISSING) status=STATUS_INVALID_PARAMETER;
    }
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

// A capacity-accepted physical table fill changes logical construction state,
// not the live CPU/GPU table. No allocation and no hardware write here.
NTSTATUS VidMmCommitPagingFill(ULONGLONG Physical, ULONGLONG Bytes, ULONG Pattern)
{
    BC250_VIDMM* vm=&g_VidMm;
    NTSTATUS status=STATUS_SUCCESS;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || !vm->Shadow.Slots) status=STATUS_DEVICE_NOT_READY;
    else if (Physical<vm->SegmentPhysical || Physical-vm->SegmentPhysical>=vm->SegmentLength ||
        Bytes>vm->SegmentLength-(Physical-vm->SegmentPhysical) ||
        PagingPtShadowFill(&vm->Shadow,Physical,Bytes,Pattern)!=PAGING_PT_OK) status=STATUS_INVALID_PARAMETER;
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS VidMmCommitPagingTransfer(const BC250_PAGING_COPY_SLICE* Slice)
{
    BC250_VIDMM* vm=&g_VidMm;
    NTSTATUS status=STATUS_SUCCESS;
    int result;
    if (!Slice || Slice->DestinationSystem || !Slice->Bytes) return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&vm->CpuUpdateLock);
    if (!vm->Ready || !vm->Write || !vm->Shadow.Slots) status=STATUS_DEVICE_NOT_READY;
    else if (Slice->DestinationPhysical<vm->SegmentPhysical ||
        Slice->DestinationPhysical-vm->SegmentPhysical>=vm->SegmentLength ||
        Slice->Bytes>vm->SegmentLength-(Slice->DestinationPhysical-vm->SegmentPhysical))
        status=STATUS_INVALID_PARAMETER;
    else {
        result=PagingPtShadowCopyBytes(&vm->Shadow,Slice->SourcePhysical,Slice->DestinationPhysical,
            Slice->Bytes,!Slice->SourceSystem);
        if (result!=PAGING_PT_OK && result!=PAGING_PT_MISSING) status=STATUS_INVALID_PARAMETER;
    }
    ExReleasePushLockExclusive(&vm->CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

// A leaf may name application storage or a table mapped as paging-process data.
static BOOLEAN VidMmLocalPageAllowed(const BC250_VIDMM* Vm, ULONGLONG Physical)
{
    return (Vm->Pte.vram_size>=PAGE_SIZE && Physical>=Vm->Pte.vram_base &&
            Physical-Vm->Pte.vram_base<=Vm->Pte.vram_size-PAGE_SIZE) ||
           (Vm->Pte.table_size>=PAGE_SIZE && Physical>=Vm->Pte.table_base &&
            Physical-Vm->Pte.table_base<=Vm->Pte.table_size-PAGE_SIZE);
}

static NTSTATUS VidMmUpdatePageTableLocked(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update)
{
    BC250_VIDMM* vm = &g_VidMm;
    UINT level, i;
    BOOLEAN cpu, logCall;
    ULONGLONG tableOffset,tablePhysical=0;
    int shadowStatus=PAGING_PT_MISSING;
    ULONGLONG* entries = vm->CpuEntries;
    volatile ULONGLONG* table = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    if (Update == NULL) return STATUS_INVALID_PARAMETER;
    if (!vm->Ready || !vm->Write || vm->SegmentLength < PAGE_SIZE) return STATUS_DEVICE_NOT_READY;
    level = Update->PageTableLevel;
    cpu = Update->UpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;
    tableOffset = cpu ? 0 : Update->PageTableAddress.GpuPhysical.SegmentOffset;
    if (level >= BC250_VIDMM_LEVELS || Update->Flags.Use64KBPages ||
        Update->pPageTableEntries == NULL || Update->NumPageTableEntries == 0 ||
        Update->StartIndex >= BC250_VIDMM_PTES ||
        Update->NumPageTableEntries > BC250_VIDMM_PTES - Update->StartIndex ||
        (cpu ? (Update->PageTableAddress.CpuVirtual == NULL ||
                ((ULONG_PTR)Update->PageTableAddress.CpuVirtual & (PAGE_SIZE-1)) != 0) :
               (Update->UpdateMode != DXGK_PAGETABLEUPDATE_GPU_PHYSICAL ||
                Update->PageTableAddress.GpuPhysical.SegmentId != (vm->Pte.table_size ? vm->Pte.table_segment : vm->Pte.vram_segment) ||
                (tableOffset & (PAGE_SIZE-1)) != 0 || tableOffset > vm->SegmentLength-PAGE_SIZE))) {
        InterlockedIncrement(&vm->BadCalls);
        return STATUS_INVALID_PARAMETER;
    }
    InterlockedIncrement(&vm->Calls[level]);
    logCall = InterlockedIncrement(cpu ? &vm->CpuCalls : &vm->GpuCalls) <= BC250_VIDMM_LOG_CALLS*2;
    __try {
        for (i=0; i<Update->NumPageTableEntries; i++) {
            const DXGK_PTE* pte = Update->pPageTableEntries + (Update->Flags.Repeat ? 0 : i);
            if (bc250_pte_from_dxgk(&vm->Pte,VidMmKind(level),pte->Flags,pte->PageAddress,&entries[i]) != 0) {
                InterlockedIncrement(&vm->Refused);
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            InterlockedIncrement64(&vm->Entries[level]);
            if (pte->Flags & BC250_DXGK_PTE_VALID) InterlockedIncrement64(&vm->Valid[level]);
        }
        if (NT_SUCCESS(status)) {
            if (cpu) table = (volatile ULONGLONG*)Update->PageTableAddress.CpuVirtual;
            else {
                if (vm->SegmentMapping == NULL) status = STATUS_DEVICE_NOT_READY;
                else table = (volatile ULONGLONG*)(vm->SegmentMapping+(SIZE_T)tableOffset);
            }
            if (table != NULL) {
                // CPU_VIRTUAL borrows a valid mapping of a pinned local table.
                // Obtain only its identity for the logical index, not a DMA address.
                // Do not retain the borrowed pointer after this callback.
                tablePhysical=cpu ? (ULONGLONG)MmGetPhysicalAddress((PVOID)table).QuadPart : vm->SegmentPhysical+tableOffset;
                // A borrowed CPU mapping must still name a whole local table
                // inside the configured table extent before either write occurs.
                if ((tablePhysical & (PAGE_SIZE-1))!=0 || tablePhysical<vm->SegmentPhysical ||
                    tablePhysical-vm->SegmentPhysical>vm->SegmentLength-PAGE_SIZE)
                    shadowStatus=PAGING_PT_INVALID;
                else shadowStatus=PagingPtShadowCanApply(&vm->Shadow,tablePhysical,Update->StartIndex,
                                                        Update->NumPageTableEntries,cpu);
                // CPU_VIRTUAL is paging-process initialization. An unregistered
                // GPU_PHYSICAL bootstrap table belongs to another process.
                if (shadowStatus!=PAGING_PT_OK && !(shadowStatus==PAGING_PT_MISSING && !cpu)) {
                    status=shadowStatus==PAGING_PT_FULL ? STATUS_INSUFFICIENT_RESOURCES : STATUS_INVALID_PARAMETER;
                    table=NULL; // preflight fails before either destination is modified
                }
            }
            if (table != NULL) {
                for (i=0; i<Update->NumPageTableEntries; i++) {
                    table[Update->StartIndex+i] = entries[i];
                    InterlockedIncrement64(&vm->Written);
                }
                KeMemoryBarrier();
                if (shadowStatus==PAGING_PT_OK)
                    (void)PagingPtShadowApply(&vm->Shadow,tablePhysical,Update->StartIndex,
                                             Update->NumPageTableEntries,entries,cpu);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The OS must keep kernel pointers valid. This handles catchable faults,
        // not rollback after an invalid destination pointer faults mid-write.
        status = GetExceptionCode();
    }
    if (!NT_SUCCESS(status)) InterlockedIncrement(&vm->BadCalls);
    if (logCall || !NT_SUCCESS(status))
        GuardLog("vidmm: CPU update level %u mode %u start %u count %u status 0x%08X",level,
                 (ULONG)Update->UpdateMode,Update->StartIndex,Update->NumPageTableEntries,status);
    return status;
}

// PASSIVE_LEVEL. Lock order: optional device GfxPagingLock first, then this
// snapshot lock. No path under CpuUpdateLock acquires the device lock. Stop joins
// active CPU updates before closing gates. PnP serializes start/reinitialization.
NTSTATUS VidMmUpdatePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update)
{
    NTSTATUS status;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_VidMm.CpuUpdateLock);
    status = VidMmUpdatePageTableLocked(Update);
    ExReleasePushLockExclusive(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    return status;
}

BOOLEAN GfxPagingMdlAddress(_In_ PMDL Mdl, ULONG FirstPage, ULONGLONG ByteOffset,
                           ULONG Bytes, _Out_ ULONGLONG* Address)
{
    ULONGLONG begin,end,start,pages;
    if (Address==NULL) return FALSE;
    *Address=0;
    if (Mdl==NULL || sizeof(PFN_NUMBER)!=sizeof(PAGING_U64)) return FALSE;
    begin=MmGetMdlByteOffset(Mdl);
    if (begin>=PAGE_SIZE || MmGetMdlByteCount(Mdl)==0) return FALSE;
    end=begin+(ULONGLONG)MmGetMdlByteCount(Mdl);
    pages=(end+PAGE_SIZE-1)>>PAGE_SHIFT;
    start=(ULONGLONG)FirstPage<<PAGE_SHIFT;
    if (ByteOffset>MAXULONGLONG-start) return FALSE;
    start+=ByteOffset;
    if (!Bytes || start<begin || start>=end || Bytes>end-start) return FALSE;
    return PagingPageListAddress((const PAGING_U64*)MmGetMdlPfnArray(Mdl),
        (unsigned)pages,0,0,FirstPage,ByteOffset,Bytes,Address)!=0;
}

typedef struct _BC250_PAGING_STREAM {
    BC250_DEVICE* Device;
    BC250_GFX* Gfx;
    ULONGLONG Root;
    BOOLEAN Fill;
    ULONG Pattern;
    ULONGLONG StagingMc;
    ULONG CommandOffset;
    unsigned* Payload;
    BC250_WDDM_PAGING_UNSUPPORTED Unsupported;
} BC250_PAGING_STREAM;

static int PagingResolve(void* Context, PAGING_U64 Va, unsigned Bytes, PAGING_U64* Mc)
{
    BC250_PAGING_STREAM* stream = (BC250_PAGING_STREAM*)Context;
    ULONGLONG physical = 0;
    BOOLEAN system = FALSE;
    if (!VidMmTranslatePaging(stream->Root, Va, &physical, &system)) {
        stream->Unsupported = BC250PagingNoTranslation; return 0;
    }
    if (system) {
        ULONGLONG page=physical & ~(ULONGLONG)(PAGE_SIZE-1);
        if (!stream->Gfx->PagingWindowReady || (page & ~AMDGPU_PTE_ADDR_MASK) != 0) {
            stream->Unsupported = BC250PagingSystemMemory; return 0;
        }
        *Mc=physical | PAGING_SYSTEM_ADDRESS; return 1;
    }
    if (!PagingPhysicalToMc(physical, Bytes, (ULONGLONG)stream->Device->VramPhysical.QuadPart,
                            stream->Device->VramMcBase, stream->Device->VramLength, Mc)) {
        stream->Unsupported = BC250PagingNoTranslation; return 0;
    }
    return 1;
}

static int PagingEmit(void* Context, unsigned* Buffer, unsigned Capacity, PAGING_U64 Src,
                      PAGING_U64 Dst, unsigned Bytes, unsigned* Written)
{
    BC250_PAGING_STREAM* stream = (BC250_PAGING_STREAM*)Context;
    if ((Src | Dst) & PAGING_SYSTEM_ADDRESS) {
        struct amdgpu_device* adev=stream->Gfx->PagingDevicePtr;
        struct bc250_sdma_paging_mapping map;
        u64 ptes[2]={0,0};
        RtlZeroMemory(&map,sizeof(map));
        map.table_mc=stream->Gfx->PagingWindow.table;
        map.ptes=ptes; map.page_count=2;
        map.scratch_mc=bc250_sdma_fence_addr(adev,BC250_PAGING_MARKER_SLOT);
        if (map.scratch_mc==0) return BC250_SDMA_PAGING_EINVAL;
        // Command position is unique within the OS buffer, including earlier build calls.
        // Submit resets scratch only after the previous actual hardware fence.
        map.first_sequence=1u+3u*(stream->CommandOffset/4u+(unsigned)(Buffer-stream->Payload));
        map.src_mc=Src; map.dst_mc=Dst; map.bytes=Bytes;
        map.fill=stream->Fill; map.pattern=stream->Pattern;map.staging_mc=stream->StagingMc;
        if (Src & PAGING_SYSTEM_ADDRESS) {
            u64 physical=Src & ~PAGING_SYSTEM_ADDRESS;
            ptes[0]=bc250_gart_pte(physical & ~(u64)4095,bc250_gart_pte_flags(adev));
            map.src_mc=stream->Gfx->PagingWindow.mc+(physical & 4095);
        }
        if (Dst & PAGING_SYSTEM_ADDRESS) {
            u64 physical=Dst & ~PAGING_SYSTEM_ADDRESS;
            ptes[1]=bc250_gart_pte(physical & ~(u64)4095,bc250_gart_pte_flags(adev));
            map.dst_mc=stream->Gfx->PagingWindow.mc+PAGE_SIZE+(physical & 4095);
        }
        return bc250_sdma_paging_mapped_transfer(adev,Buffer,Capacity,&map,Written);
    }
    return stream->Fill ? bc250_sdma_paging_fill(stream->Gfx->PagingDevicePtr, Buffer, Capacity,
                                               Dst, stream->Pattern, Bytes, Written)
                        : bc250_sdma_paging_copy(stream->Gfx->PagingDevicePtr, Buffer, Capacity,
                                               Src, Dst, Bytes, Written);
}

typedef struct _BC250_PHYSICAL_STREAM {
    BC250_PAGING_STREAM Common; // First member: PagingEmit uses the common state.
    const BC250_PAGING_ENDPOINT* Source;
    const BC250_PAGING_ENDPOINT* Destination;
} BC250_PHYSICAL_STREAM;

static int PagingResolvePhysical(BC250_PHYSICAL_STREAM* Stream,
    const BC250_PAGING_ENDPOINT* Endpoint, PAGING_U64 Address, unsigned Bytes, PAGING_U64* Mc)
{
    ULONGLONG offset,physical;
    *Mc=0;
    if (Endpoint==NULL) return 0;
    if (Endpoint->Mdl!=NULL) offset=Address;
    else {
        if (Address<Endpoint->Address) return 0;
        offset=Address-Endpoint->Address;
    }
    if (!Bytes || offset>=Endpoint->Length || Bytes>Endpoint->Length-offset) return 0;
    if (Endpoint->Aperture) {
        if (Endpoint->Mdl || !Stream->Common.Gfx->PagingWindowReady ||
            !VidMmResolveAperture(Address,Bytes,&physical) ||
            ((physical & ~(ULONGLONG)(PAGE_SIZE-1)) & ~AMDGPU_PTE_ADDR_MASK)!=0) return 0;
        *Mc=physical | PAGING_SYSTEM_ADDRESS;
    } else if (Endpoint->Mdl!=NULL) {
        if (!Stream->Common.Gfx->PagingWindowReady ||
            !GfxPagingMdlAddress(Endpoint->Mdl,Endpoint->FirstPage,offset,Bytes,&physical) ||
            ((physical & ~(ULONGLONG)(PAGE_SIZE-1)) & ~AMDGPU_PTE_ADDR_MASK)!=0) return 0;
        *Mc=physical | PAGING_SYSTEM_ADDRESS;
    } else {
        ULONGLONG base=Stream->Common.Device->VramMcBase;
        ULONGLONG length=Stream->Common.Device->VramLength;
        if (Address<base || Address-base>=length || Bytes>length-(Address-base) ||
            (Address & PAGING_SYSTEM_ADDRESS)!=0) return 0;
        *Mc=Address;
    }
    return 1;
}

static int PagingResolveSource(void* Context, PAGING_U64 Address, unsigned Bytes, PAGING_U64* Mc)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    return PagingResolvePhysical(stream,stream->Source,Address,Bytes,Mc);
}

static int PagingResolveDestination(void* Context, PAGING_U64 Address, unsigned Bytes, PAGING_U64* Mc)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    return PagingResolvePhysical(stream,stream->Destination,Address,Bytes,Mc);
}

static int PagingSourceIdentity(void* Context,PAGING_U64 Address,unsigned Bytes,PAGING_U64* Physical)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    PAGING_U64 mc;
    if (!PagingResolveSource(Context,Address,Bytes,&mc)) return 0;
    if (mc&PAGING_SYSTEM_ADDRESS) *Physical=mc&~PAGING_SYSTEM_ADDRESS;
    else *Physical=mc-stream->Common.Device->VramMcBase+(ULONGLONG)stream->Common.Device->VramPhysical.QuadPart;
    return 1;
}
static int PagingDestinationIdentity(void* Context,PAGING_U64 Address,unsigned Bytes,PAGING_U64* Physical)
{
    BC250_PHYSICAL_STREAM* stream=(BC250_PHYSICAL_STREAM*)Context;
    PAGING_U64 mc;
    if (!PagingResolveDestination(Context,Address,Bytes,&mc)) return 0;
    if (mc&PAGING_SYSTEM_ADDRESS) *Physical=mc&~PAGING_SYSTEM_ADDRESS;
    else *Physical=mc-stream->Common.Device->VramMcBase+(ULONGLONG)stream->Common.Device->VramPhysical.QuadPart;
    return 1;
}

// Classification only. Workspace never reaches hardware and is released before
// packets are built. Sorting physical spans avoids quadratic all-pairs PFN checks.
NTSTATUS GfxPagingCheckDisjoint(BC250_DEVICE* Device,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,BOOLEAN* Disjoint)
{
    BC250_PHYSICAL_STREAM stream;BC250_GFX* gfx;PAGING_INTERVAL* work;
    ULONGLONG src=Source->Mdl?0:Source->Address,dst=Destination->Mdl?0:Destination->Address;
    unsigned count=PagingIntervalCapacity(src,Bytes);int separate=0;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Disjoint=FALSE;
    if (!count) return status;
    work=(PAGING_INTERVAL*)ExAllocatePool2(POOL_FLAG_PAGED,(SIZE_T)count*sizeof(*work),BC250_GFX_TAG);
    if (!work) return STATUS_INSUFFICIENT_RESOURCES;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (gfx && gfx->PagingReady && gfx->PagingWindowReady) {
        RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
        stream.Source=Source;stream.Destination=Destination;
        if (PagingIntervalsDisjoint(&stream,PagingSourceIdentity,PagingDestinationIdentity,
                src,dst,Bytes,work,count,&separate)) {status=STATUS_SUCCESS;*Disjoint=(BOOLEAN)separate;}
    }
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    ExFreePoolWithTag(work,BC250_GFX_TAG);
    return status;
}

// Complete cycles of system-page permutations fit in each submission. Scratch is
// driver-owned through engine retirement; no value must survive another paging
// job. Oversized cycles use bounded swaps; partial-page aliases remain separate.
NTSTATUS GfxPagingBuildPermutation(BC250_DEVICE* Device,const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination,ULONGLONG Bytes,PVOID Buffer,
    ULONG Offset,ULONG Free,unsigned Resume,ULONG* Written,unsigned* NextResume)
{
    BC250_PHYSICAL_STREAM stream;
    BC250_GFX* gfx;
    PAGING_U64 *sources,*destinations;
    PAGING_PAGE_IDENTITY* identities;
    PAGING_PAGE_MOVE* moves;
    unsigned *permutation,*inverse;
    unsigned char *storage,*seen;
    unsigned pages,moveCapacity,moveCount=0,i,required=0,budget,maxBudget,used=0;
    unsigned first=0,last=0,cycleSize=0,nextResume=Resume;
    int batch;
    ULONGLONG src,dst,scratchPhysical;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int result;
    *Written=0;*NextResume=Resume;
    if(!Device || !Source || !Destination || !Buffer || !Bytes || (Bytes&4095) ||
       (Offset&3) || Offset>BC250_GFX_PAGING_BUFFER_BYTES || Bytes/PAGE_SIZE>0x2aaaaaaau ||
       !(Source->Mdl || Source->Aperture) || !(Destination->Mdl || Destination->Aperture))return status;
    src=Source->Mdl?0:Source->Address;dst=Destination->Mdl?0:Destination->Address;
    if((src|dst)&4095)return status;
    if(!GfxPagingEndpointValid(Device,Source,Bytes) || !GfxPagingEndpointValid(Device,Destination,Bytes))return status;
    pages=(unsigned)(Bytes/PAGE_SIZE);moveCapacity=pages*3;
    if(Resume && Resume!=pages && !(Resume&PAGING_PERMUTATION_RESUME))return status;
    // 65 bytes/page at most; all typed arrays precede byte flags and stay aligned.
    storage=ExAllocatePool2(POOL_FLAG_PAGED,(SIZE_T)pages*80,BC250_GFX_TAG);
    if(!storage)return STATUS_INSUFFICIENT_RESOURCES;
    sources=(PAGING_U64*)storage;destinations=sources+pages;
    identities=(PAGING_PAGE_IDENTITY*)(destinations+pages);
    permutation=(unsigned*)(identities+pages);inverse=permutation+pages;
    moves=(PAGING_PAGE_MOVE*)(inverse+pages);seen=(unsigned char*)(moves+moveCapacity);
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if(!gfx || !gfx->PagingReady || !gfx->PagingWindowReady || !gfx->PagingRing ||
       !gfx->PagingDevicePtr || gfx->PagingCopyStaging.size<PAGE_SIZE)goto Done;
    if(gfx->PagingCopyStaging.mc<Device->VramMcBase ||
       gfx->PagingCopyStaging.mc-Device->VramMcBase>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart)goto Done;
    scratchPhysical=gfx->PagingCopyStaging.mc-Device->VramMcBase+(ULONGLONG)Device->VramPhysical.QuadPart;
    RtlZeroMemory(&stream,sizeof(stream));stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Payload=(unsigned*)Buffer;stream.Common.CommandOffset=Offset;
    stream.Source=Source;stream.Destination=Destination;
    for(i=0;i<pages;i++) {
        if(!PagingSourceIdentity(&stream,src+(ULONGLONG)i*PAGE_SIZE,PAGE_SIZE,sources+i) ||
           !PagingDestinationIdentity(&stream,dst+(ULONGLONG)i*PAGE_SIZE,PAGE_SIZE,destinations+i) ||
           sources[i]==scratchPhysical || destinations[i]==scratchPhysical)goto Done;
    }
    if(!PagingPermutationNormalize(sources,destinations,pages,identities,seen,permutation))goto Done;
    if(Resume==pages){*NextResume=pages;status=STATUS_SUCCESS;goto Done;}
    // Every action contains at least one system endpoint, using the same two-PTE
    // transaction. Ask the real packet builder for its aligned cost without writes.
    result=PagingEmit(&stream,(unsigned*)Buffer,0,sources[0]|PAGING_SYSTEM_ADDRESS,
        gfx->PagingCopyStaging.mc,PAGE_SIZE,&required);
    if(result!=BC250_SDMA_PAGING_INSUFFICIENT || !required)goto Done;
    maxBudget=PagingStreamCapacity(BC250_GFX_PAGING_BUFFER_BYTES,0,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if(maxBudget/required<3){status=STATUS_NOT_SUPPORTED;goto Done;}
    if(!PagingPermutationPlanBounded(permutation,pages,inverse,seen,moves,moveCapacity,
                                    maxBudget/required,&moveCount))goto Done;
    first=Resume&PAGING_PERMUTATION_RESUME ? Resume&~PAGING_PERMUTATION_RESUME : 0;
    if(first>moveCount || (first==moveCount && Resume))goto Done;
    if(!moveCount){*NextResume=pages;status=STATUS_SUCCESS;goto Done;}
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    batch=PagingPermutationBatch(moves,moveCount,first,budget/required,&last,&cycleSize);
    if(batch==PagingPermutationInvalid)goto Done;
    if(batch==PagingPermutationNeedCycle){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    nextResume=last==moveCount?pages:PAGING_PERMUTATION_RESUME|last;
    for(i=first;i<last;i++) {
        PAGING_U64 from=moves[i].source==PAGING_PERMUTATION_SCRATCH ? gfx->PagingCopyStaging.mc :
            sources[moves[i].source]|PAGING_SYSTEM_ADDRESS;
        PAGING_U64 to=moves[i].destination==PAGING_PERMUTATION_SCRATCH ? gfx->PagingCopyStaging.mc :
            sources[moves[i].destination]|PAGING_SYSTEM_ADDRESS;
        unsigned written=0;
        // StagingMc stays zero: these are distinct whole-page copies. The cycle
        // owns PagingCopyStaging; no per-slice snapshot is allowed to overwrite it.
        result=PagingEmit(&stream,(unsigned*)Buffer+used,budget-used,from,to,PAGE_SIZE,&written);
        if(result!=BC250_SDMA_PAGING_OK || !written || written>required)goto Done;
        used+=written;
    }
    *Written=used;*NextResume=nextResume;
    status=nextResume==pages?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    ExFreePoolWithTag(storage,BC250_GFX_TAG);return status;
}

// Validate the complete requested interval before the first batch can escape.
// O(1): PFN contents are still checked page by page under the OS MDL lifetime.
BOOLEAN GfxPagingEndpointValid(const BC250_DEVICE* Device,
    const BC250_PAGING_ENDPOINT* Endpoint, ULONGLONG Bytes)
{
    ULONGLONG start,end,begin;
    if (!Endpoint || !Bytes || Bytes>Endpoint->Length) return FALSE;
    if (Endpoint->Aperture) {
        ULONGLONG base=Device->WddmAperture.mc,length=Device->WddmAperture.bytes;
        return !Endpoint->Mdl && Endpoint->Address>=base && Endpoint->Address-base<length &&
            Bytes<=length-(Endpoint->Address-base) && Bytes<=MAXULONGLONG-Endpoint->Address;
    }
    if (Endpoint->Mdl) {
        begin=MmGetMdlByteOffset(Endpoint->Mdl);
        if (sizeof(PFN_NUMBER)!=sizeof(PAGING_U64) ||
            begin>=PAGE_SIZE || !MmGetMdlByteCount(Endpoint->Mdl)) return FALSE;
        end=begin+(ULONGLONG)MmGetMdlByteCount(Endpoint->Mdl);
        start=(ULONGLONG)Endpoint->FirstPage<<PAGE_SHIFT;
        return start>=begin && start<end && Bytes<=end-start;
    }
    start=Endpoint->Address;
    if (start<Device->VramMcBase || start-Device->VramMcBase>=Device->VramLength ||
        Bytes>Device->VramLength-(start-Device->VramMcBase) ||
        Bytes>MAXULONGLONG-start) return FALSE;
    // The high bit is reserved for the internal system-page route marker.
    return ((start | (start+Bytes-1)) & PAGING_SYSTEM_ADDRESS)==0;
}

// PASSIVE_LEVEL. Endpoint selection/segment bounds and external resume encoding
// belong to the WDDM adapter. This builder never maps MDLs or takes ownership.
NTSTATUS GfxPagingBuildPhysical(_Inout_ BC250_DEVICE* Device,
    _In_opt_ const BC250_PAGING_ENDPOINT* Source, _In_ const BC250_PAGING_ENDPOINT* Destination,
    BOOLEAN Fill, ULONGLONG Bytes, ULONG Pattern, _Inout_ PVOID DmaBuffer,
    ULONG DmaBufferOffset, ULONG DmaBufferFree, ULONGLONG StartByte,
    _Out_ ULONG* DwordsWritten, _Out_ ULONGLONG* NextByte)
{
    BC250_PHYSICAL_STREAM stream;
    BC250_GFX* gfx;
    unsigned budget,written=0;
    PAGING_U64 next=StartByte,src=0,dst;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int result;
    *DwordsWritten=0; *NextByte=StartByte;
    if (!DmaBuffer || !Destination || (!Fill && !Source) || !Bytes || StartByte>Bytes ||
        !GfxPagingEndpointValid(Device,Destination,Bytes) ||
        (!Fill && !GfxPagingEndpointValid(Device,Source,Bytes)) ||
        (DmaBufferOffset & 3u)!=0 || DmaBufferOffset>BC250_GFX_PAGING_BUFFER_BYTES)
        return status;
    dst=Destination->Mdl ? 0 : Destination->Address;
    if (!Fill) src=Source->Mdl ? 0 : Source->Address;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingDevicePtr || !gfx->PagingRing) goto Done;
    RtlZeroMemory(&stream,sizeof(stream));
    stream.Common.Device=Device;stream.Common.Gfx=gfx;
    stream.Common.Fill=Fill;stream.Common.Pattern=Pattern;
    stream.Common.Payload=(unsigned*)DmaBuffer;stream.Common.CommandOffset=DmaBufferOffset;
    stream.Source=Source;stream.Destination=Destination;
    budget=PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=PagingStreamBuildEndpoints64(&stream,PagingResolveSource,PagingResolveDestination,
        PagingEmit,Fill,src,dst,Bytes,StartByte,(unsigned*)DmaBuffer,budget,&written,&next);
    if (result!=PagingStreamDone && result!=PagingStreamMore) goto Done;
    *DwordsWritten=written;*NextByte=next;
    status=result==PagingStreamMore ? STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER : STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// Capture the same identities used by this one-page packet transaction. No
// shadow change occurs here; publication owns the later logical commit.
static NTSTATUS PagingBuildCopyPageCore(BC250_DEVICE* Device, ULONGLONG Root, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    BC250_PHYSICAL_STREAM stream;
    BC250_PAGING_COPY_SLICE candidate;
    BC250_GFX* gfx;
    PAGING_U64 src,dst,srcMc,dstMc;
    unsigned budget,written=0;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int overlap,result;
    *Written=0;RtlZeroMemory(Slice,sizeof(*Slice));
    if (!Buffer || !Source || !Destination || !Bytes || Progress>MAXULONGLONG-Bytes ||
        (Offset&3)!=0 || Offset>BC250_GFX_PAGING_BUFFER_BYTES) return status;
    if (Root) {
        if (Source->Mdl || Destination->Mdl ||
            Source->Address>MAXULONGLONG-(Progress+Bytes) ||
            Destination->Address>MAXULONGLONG-(Progress+Bytes)) return status;
    } else if (!GfxPagingEndpointValid(Device,Source,Progress+Bytes) ||
               !GfxPagingEndpointValid(Device,Destination,Progress+Bytes)) return status;
    src=(Source->Mdl?0:Source->Address)+Progress;
    dst=(Destination->Mdl?0:Destination->Address)+Progress;
    if (Bytes>PAGE_SIZE-(src&(PAGE_SIZE-1)) || Bytes>PAGE_SIZE-(dst&(PAGE_SIZE-1))) return status;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr) goto Done;
    RtlZeroMemory(&stream,sizeof(stream));RtlZeroMemory(&candidate,sizeof(candidate));
    stream.Common.Device=Device;stream.Common.Gfx=gfx;stream.Common.Payload=(unsigned*)Buffer;
    stream.Common.CommandOffset=Offset;stream.Source=Source;stream.Destination=Destination;
    stream.Common.Root=Root;
    if (Root) {
        if (!PagingResolve(&stream.Common,src,Bytes,&srcMc) ||
            !PagingResolve(&stream.Common,dst,Bytes,&dstMc)) goto Done;
    } else if (!PagingResolveSource(&stream,src,Bytes,&srcMc) ||
               !PagingResolveDestination(&stream,dst,Bytes,&dstMc)) goto Done;
    candidate.SourceSystem=(srcMc&PAGING_SYSTEM_ADDRESS)!=0;
    candidate.DestinationSystem=(dstMc&PAGING_SYSTEM_ADDRESS)!=0;
    candidate.SourcePhysical=candidate.SourceSystem ? srcMc&~PAGING_SYSTEM_ADDRESS : srcMc-Device->VramMcBase;
    candidate.DestinationPhysical=candidate.DestinationSystem ? dstMc&~PAGING_SYSTEM_ADDRESS : dstMc-Device->VramMcBase;
    if (!candidate.SourceSystem) {
        if (candidate.SourcePhysical>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) goto Done;
        candidate.SourcePhysical+=(ULONGLONG)Device->VramPhysical.QuadPart;
    }
    if (!candidate.DestinationSystem) {
        if (candidate.DestinationPhysical>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) goto Done;
        candidate.DestinationPhysical+=(ULONGLONG)Device->VramPhysical.QuadPart;
    }
    overlap=candidate.SourcePhysical<=candidate.DestinationPhysical ?
        candidate.DestinationPhysical-candidate.SourcePhysical<Bytes :
        candidate.SourcePhysical-candidate.DestinationPhysical<Bytes;
    // Preserve aliased system data while both GART mappings remain active.
    if ((overlap || ForceSnapshot) && (candidate.SourceSystem || candidate.DestinationSystem)) {
        if (gfx->PagingCopyStaging.size<PAGE_SIZE || !gfx->PagingCopyStaging.mc) goto Done;
        stream.Common.StagingMc=gfx->PagingCopyStaging.mc;
    }
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    if ((overlap || ForceSnapshot) && !candidate.SourceSystem && !candidate.DestinationSystem) {
        ULONGLONG marker=bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT);
        if (!marker || gfx->PagingCopyStaging.size<PAGE_SIZE) goto Done;
        result=bc250_sdma_paging_copy_bytes(gfx->PagingDevicePtr,(u32*)Buffer,budget,srcMc,dstMc,Bytes,
            gfx->PagingCopyStaging.mc,marker,1u+3u*(Offset/4u),&written);
    } else result=PagingEmit(&stream,(unsigned*)Buffer,budget,srcMc,dstMc,Bytes,&written);
    if (result==BC250_SDMA_PAGING_INSUFFICIENT) {status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    if (result!=BC250_SDMA_PAGING_OK) goto Done;
    candidate.Bytes=Bytes;*Slice=candidate;*Written=written;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS GfxPagingBuildCopyPageEx(BC250_DEVICE* Device, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    return PagingBuildCopyPageCore(Device,0,Source,Destination,Progress,Bytes,ForceSnapshot,
        Buffer,Offset,Free,Written,Slice);
}

// Resolve both VAs under the same engine lifetime lock and return exactly the
// identities encoded in the packet. Whole-transfer ordering belongs to the caller.
NTSTATUS GfxPagingBuildVirtualCopyPage(BC250_DEVICE* Device, ULONGLONG Root,
    ULONGLONG SourceVa, ULONGLONG DestinationVa, ULONG Bytes, BOOLEAN ForceSnapshot,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    BC250_PAGING_ENDPOINT source,destination;
    *Written=0;RtlZeroMemory(Slice,sizeof(*Slice));
    if (!Root) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&source,sizeof(source));RtlZeroMemory(&destination,sizeof(destination));
    source.Address=SourceVa;destination.Address=DestinationVa;
    return PagingBuildCopyPageCore(Device,Root,&source,&destination,0,Bytes,ForceSnapshot,
        Buffer,Offset,Free,Written,Slice);
}

NTSTATUS GfxPagingBuildCopyPage(BC250_DEVICE* Device, const BC250_PAGING_ENDPOINT* Source,
    const BC250_PAGING_ENDPOINT* Destination, ULONGLONG Progress, ULONG Bytes,
    PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, BC250_PAGING_COPY_SLICE* Slice)
{
    return GfxPagingBuildCopyPageEx(Device,Source,Destination,Progress,Bytes,FALSE,
        Buffer,Offset,Free,Written,Slice);
}

// Resolve exactly one page slice and return the identity used by its packets.
// The caller publishes its logical table effect before constructing another slice.
NTSTATUS GfxPagingBuildFillPage(BC250_DEVICE* Device, ULONGLONG Root, ULONGLONG Va,
    ULONG Bytes, ULONG Pattern, PVOID Buffer, ULONG Offset, ULONG Free,
    ULONG* Written, ULONGLONG* Physical, BOOLEAN* System)
{
    BC250_PAGING_STREAM stream;
    BC250_GFX* gfx;
    PAGING_U64 mc;
    unsigned budget,written=0;
    int result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    *Written=0;*Physical=0;*System=FALSE;
    if (!Root || !Buffer || !Bytes || Bytes>PAGE_SIZE-(Va&(PAGE_SIZE-1)) ||
        ((Va|Bytes|Offset)&3)!=0 || Va>MAXULONGLONG-Bytes || Offset>BC250_GFX_PAGING_BUFFER_BYTES)
        return status;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingDevicePtr || !gfx->PagingRing) goto Done;
    RtlZeroMemory(&stream,sizeof(stream));
    stream.Device=Device;stream.Gfx=gfx;stream.Root=Root;stream.Fill=TRUE;stream.Pattern=Pattern;
    stream.Payload=(unsigned*)Buffer;stream.CommandOffset=Offset;
    if (!PagingResolve(&stream,Va,Bytes,&mc)) goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=PagingEmit(&stream,(unsigned*)Buffer,budget,0,mc,Bytes,&written);
    if (result==BC250_SDMA_PAGING_INSUFFICIENT) { status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done; }
    if (result!=BC250_SDMA_PAGING_OK) goto Done;
    *System=(mc&PAGING_SYSTEM_ADDRESS)!=0;
    *Physical=*System ? mc&~PAGING_SYSTEM_ADDRESS :
        (ULONGLONG)Device->VramPhysical.QuadPart+(mc-Device->VramMcBase);
    *Written=written;status=STATUS_SUCCESS;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// Build one permanent aperture batch. OS MDLs describe page identities here,
// including partial boundary pages; byte-copy interval rules do not apply.
NTSTATUS GfxPagingBuildAperture(BC250_DEVICE* Device, const BC250_PAGING_APERTURE_OP* Operation,
    ULONG StartPage, PVOID Buffer, ULONG Offset, ULONG Free, ULONG* Written, ULONG* NextPage)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev;
    PAGING_APERTURE live;
    ULONGLONG mc,table,pages=0,physical,flags,marker;
    u64 entries[256];
    unsigned budget,count,i,written=0;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    int result;
    *Written=0;*NextPage=StartPage;
    if (!Device || !Operation || !Buffer || (Offset&3u) || Offset>BC250_GFX_PAGING_BUFFER_BYTES ||
        !PagingApertureRange(&Device->WddmAperture,Operation->FirstPage,Operation->PageCount,&mc,&table) ||
        StartPage>Operation->PageCount) return status;
    if (Operation->Unmap) {
        if ((Operation->DummyPhysical&~AMDGPU_PTE_ADDR_MASK)!=0) return status;
    } else {
        ULONGLONG begin,end;
        if (!Operation->Mdl || sizeof(PFN_NUMBER)!=sizeof(PAGING_U64)) return status;
        begin=MmGetMdlByteOffset(Operation->Mdl);
        if (begin>=PAGE_SIZE || !MmGetMdlByteCount(Operation->Mdl)) return status;
        end=begin+(ULONGLONG)MmGetMdlByteCount(Operation->Mdl);
        pages=(end+PAGE_SIZE-1)>>PAGE_SHIFT;
        if (Operation->MdlOffset>=pages || Operation->PageCount>pages-Operation->MdlOffset) return status;
    }
    if (StartPage==Operation->PageCount) return STATUS_SUCCESS;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (!gfx || !gfx->PagingReady || !gfx->PagingRing || !gfx->PagingDevicePtr) {
        status=STATUS_DEVICE_NOT_READY;goto Done;
    }
    adev=gfx->PagingDevicePtr;
    if (!adev->gart.bo || !PagingApertureInit(adev->gmc.gart_start,adev->gmc.gart_size,
            adev->gart.bo->gpu_addr,adev->gart.table_size,&live) ||
        live.mc!=Device->WddmAperture.mc || live.table!=Device->WddmAperture.table ||
        live.bytes!=Device->WddmAperture.bytes) goto Done;
    budget=PagingStreamCapacity(Free,Offset,BC250_GFX_PAGING_BUFFER_BYTES,gfx->PagingRing->max_dw,
        gfx->PagingRing->funcs->align_mask,bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    budget &= ~gfx->PagingRing->funcs->align_mask;
    if (budget<32u) {status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;goto Done;}
    count=(budget-29u)/2u;
    if (count>RTL_NUMBER_OF(entries)) count=RTL_NUMBER_OF(entries);
    if (count>Operation->PageCount-StartPage) count=(unsigned)(Operation->PageCount-StartPage);
    flags=bc250_gart_pte_flags(adev);
    // Match amdgpu cached TT flags. Noncoherent map requests need no host snoop;
    // dummy pages retain the cached-page default, independently of previous flags.
    if (!Operation->Unmap && !Operation->CacheCoherent) flags &= ~AMDGPU_PTE_SNOOPED;
    for (i=0;i<count;i++) {
        if (Operation->Unmap) physical=Operation->DummyPhysical;
        else if (!PagingPageListAddress((const PAGING_U64*)MmGetMdlPfnArray(Operation->Mdl),
                (unsigned)pages,0,0,Operation->MdlOffset,
                ((ULONGLONG)StartPage+i)*PAGE_SIZE,PAGE_SIZE,&physical) ||
                 (physical&~AMDGPU_PTE_ADDR_MASK)!=0) goto Done;
        entries[i]=bc250_gart_pte(physical,flags);
    }
    marker=bc250_sdma_fence_addr(adev,BC250_PAGING_MARKER_SLOT);
    result=bc250_sdma_paging_set_aperture(adev,(u32*)Buffer,budget,table+(ULONGLONG)StartPage*8,
        entries,count,marker,1u+3u*(Offset/4u),&written);
    if (result==BC250_SDMA_PAGING_OK) {
        *Written=written;*NextPage=StartPage+count;
        status=*NextPage==Operation->PageCount?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    } else if (result==BC250_SDMA_PAGING_INSUFFICIENT) status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);KeLeaveCriticalRegion();
    return status;
}

NTSTATUS GfxPagingBuildUpdate(_Inout_ BC250_DEVICE* Device,
                             _In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                             _Inout_ PVOID DmaBuffer, ULONG DmaBufferOffset, ULONG DmaBufferFree,
                             ULONG StartEntry, _Out_ ULONG* DwordsWritten, _Out_ ULONG* NextEntry,
                             _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    ULONGLONG entries[480], physical, mc; // keep fixed kernel frame below4096bytes
    unsigned budget, count, written = 0;
    u64 scratch;
    int result;
    NTSTATUS status = STATUS_SUCCESS;
    *DwordsWritten = 0; *NextEntry = StartEntry; *Unsupported = BC250PagingSupported;
    if (Update == NULL || Update->NumPageTableEntries == 0 || Update->NumPageTableEntries > 512 ||
        StartEntry >= Update->NumPageTableEntries || DmaBuffer == NULL ||
        (DmaBufferOffset & 3u) != 0 || DmaBufferOffset > BC250_GFX_PAGING_BUFFER_BYTES)
        return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL) { *Unsupported = BC250PagingNotReady; status = STATUS_DEVICE_NOT_READY; goto Done; }
    if (gfx->PagingCpuBootstrap) {
        if (StartEntry != 0 || !VidMmEncodePageTable(Update,0,1,&physical,entries)) {
            status = STATUS_INVALID_PARAMETER; goto Done;
        }
        status = VidMmUpdatePageTable(Update);
        if (NT_SUCCESS(status)) *NextEntry = Update->NumPageTableEntries;
        goto Done;
    }
    if (!gfx->PagingReady || gfx->PagingDevicePtr == NULL) {
        *Unsupported = BC250PagingNotReady; status = STATUS_DEVICE_NOT_READY; goto Done;
    }
    budget = PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
                                  gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
                                  bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    budget &= ~gfx->PagingRing->funcs->align_mask;
    if (budget < 16) { status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; goto Done; }
    count = (budget - 14u) / 2u;
    if (count > RTL_NUMBER_OF(entries)) count = RTL_NUMBER_OF(entries);
    if (count > Update->NumPageTableEntries - StartEntry) count = Update->NumPageTableEntries - StartEntry;
    if (!VidMmEncodePageTable(Update,StartEntry,count,&physical,entries) ||
        !PagingPhysicalToMc(physical,count*8u,(ULONGLONG)Device->VramPhysical.QuadPart,
                            Device->VramMcBase,Device->VramLength,&mc)) {
        status = STATUS_INVALID_PARAMETER; goto Done;
    }
    scratch = bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT);
    result = bc250_sdma_paging_update_ptes(gfx->PagingDevicePtr,(u32*)DmaBuffer,budget,mc,
                                          entries,count,scratch,1u+3u*(DmaBufferOffset/4u),&written);
    if (result == BC250_SDMA_PAGING_OK) {
        *DwordsWritten = written; *NextEntry = StartEntry + count;
        if (*NextEntry != Update->NumPageTableEntries) status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    } else if (result == BC250_SDMA_PAGING_INSUFFICIENT) status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else status = STATUS_INVALID_PARAMETER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// One complete CopyPageTableEntries range. The DDI owns list/multipass progress
// and logical publication. Both current page-table segments are local VRAM.
NTSTATUS GfxPagingBuildCopyRange(_Inout_ BC250_DEVICE* Device, ULONGLONG Root,
    _In_ const DXGK_BUILDPAGINGBUFFER_COPY_RANGE* Range, _Inout_ PVOID DmaBuffer,
    ULONG DmaBufferOffset, ULONG DmaBufferFree, _Out_ ULONG* DwordsWritten,
    _Out_ ULONGLONG* SourcePhysical, _Out_ ULONGLONG* DestinationPhysical,
    _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    ULONGLONG source,destination,srcMc,dstMc;
    BOOLEAN sourceSystem,destinationSystem;
    unsigned budget,written=0,bytes;
    int result;
    NTSTATUS status=STATUS_SUCCESS;
    *DwordsWritten=0;*SourcePhysical=0;*DestinationPhysical=0;*Unsupported=BC250PagingSupported;
    if (Range==NULL || DmaBuffer==NULL || Root==0 || !Range->NumPageTableEntries ||
        Range->SrcStartPteIndex>=512 || Range->DstStartPteIndex>=512 ||
        Range->NumPageTableEntries>512-Range->SrcStartPteIndex ||
        Range->NumPageTableEntries>512-Range->DstStartPteIndex ||
        ((Range->SrcPageTableAddress|Range->DstPageTableAddress)&65535ull)!=0 ||
        Range->SrcPageTableAddress>0xffffffffffffull-4095 ||
        Range->DstPageTableAddress>0xffffffffffffull-4095 ||
        (DmaBufferOffset&3u)!=0 || DmaBufferOffset>BC250_GFX_PAGING_BUFFER_BYTES)
        return STATUS_INVALID_PARAMETER;
    bytes=Range->NumPageTableEntries*8u;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx=(BC250_GFX*)Device->Gfx;
    if (gfx==NULL || !gfx->PagingReady || gfx->PagingDevicePtr==NULL ||
        gfx->PagingRing==NULL || gfx->PagingCopyStaging.size<PAGE_SIZE) {
        *Unsupported=BC250PagingNotReady;status=STATUS_DEVICE_NOT_READY;goto Done;
    }
    if (!VidMmTranslatePaging(Root,Range->SrcPageTableAddress+(ULONGLONG)Range->SrcStartPteIndex*8u,&source,&sourceSystem) ||
        !VidMmTranslatePaging(Root,Range->DstPageTableAddress+(ULONGLONG)Range->DstStartPteIndex*8u,&destination,&destinationSystem)) {
        *Unsupported=BC250PagingNoTranslation;status=STATUS_INVALID_PARAMETER;goto Done;
    }
    if (sourceSystem || destinationSystem) {
        *Unsupported=BC250PagingSystemMemory;status=STATUS_INVALID_PARAMETER;goto Done;
    }
    if (!PagingPhysicalToMc(source,bytes,(ULONGLONG)Device->VramPhysical.QuadPart,Device->VramMcBase,Device->VramLength,&srcMc) ||
        !PagingPhysicalToMc(destination,bytes,(ULONGLONG)Device->VramPhysical.QuadPart,Device->VramMcBase,Device->VramLength,&dstMc)) {
        *Unsupported=BC250PagingNoTranslation;status=STATUS_INVALID_PARAMETER;goto Done;
    }
    budget=PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
        gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
        bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result=bc250_sdma_paging_copy_ptes(gfx->PagingDevicePtr,(u32*)DmaBuffer,budget,srcMc,dstMc,
        Range->NumPageTableEntries,gfx->PagingCopyStaging.mc,
        bc250_sdma_fence_addr(gfx->PagingDevicePtr,BC250_PAGING_MARKER_SLOT),
        1u+3u*(DmaBufferOffset/4u),&written);
    if (result==BC250_SDMA_PAGING_OK) {
        *DwordsWritten=written;*SourcePhysical=source;*DestinationPhysical=destination;
    } else if (result==BC250_SDMA_PAGING_INSUFFICIENT) status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else status=STATUS_INVALID_PARAMETER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

// PASSIVE_LEVEL. Invalidate the entire application VMID instead of a range.
// WDDM currently binds every process to VMID1; every later root assignment also
// invalidates it. Over-invalidation avoids capturing a mutable process binding
// while this OS paging buffer waits for submission. The OS paging fence follows
// the ACK poll, so dependent work cannot observe a reported-but-unexecuted flush.
NTSTATUS GfxPagingBuildFlush(_Inout_ BC250_DEVICE* Device, ULONG Vmid,
                            _Inout_ PVOID DmaBuffer, ULONG DmaBufferOffset, ULONG DmaBufferFree,
                            _Out_ ULONG* DwordsWritten,
                            _Out_ BC250_WDDM_PAGING_UNSUPPORTED* Unsupported)
{
    BC250_GFX* gfx;
    unsigned budget, written = 0;
    int result;
    NTSTATUS status = STATUS_SUCCESS;
    *DwordsWritten = 0; *Unsupported = BC250PagingSupported;
    if (DmaBuffer == NULL || Vmid == 0 || Vmid >= 16 || (DmaBufferOffset & 3u) != 0 ||
        DmaBufferOffset > BC250_GFX_PAGING_BUFFER_BYTES) return STATUS_INVALID_PARAMETER;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&Device->GfxPagingLock);
    gfx = (BC250_GFX*)Device->Gfx;
    if (gfx == NULL || !gfx->PagingReady || gfx->PagingDevicePtr == NULL) {
        *Unsupported = BC250PagingNotReady;
        status = STATUS_DEVICE_NOT_READY;
        goto Done;
    }
    budget = PagingStreamCapacity(DmaBufferFree,DmaBufferOffset,BC250_GFX_PAGING_BUFFER_BYTES,
                                  gfx->PagingRing->max_dw,gfx->PagingRing->funcs->align_mask,
                                  bc250_sdma_fence_size(gfx->PagingRing,AMDGPU_FENCE_FLAG_INT));
    result = bc250_sdma_paging_invalidate_vmid(gfx->PagingDevicePtr,(u32*)DmaBuffer,budget,Vmid,&written);
    if (result == BC250_SDMA_PAGING_OK) *DwordsWritten = written;
    else if (result == BC250_SDMA_PAGING_INSUFFICIENT) status = STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    else status = STATUS_INVALID_PARAMETER;
Done:
    ExReleasePushLockShared(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS VidMmStartLayout(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId,
                          ULONGLONG TableOffset, ULONGLONG TableLength, ULONG TableSegmentId)
{
    RtlZeroMemory(&g_VidMm, sizeof(g_VidMm));
    ExInitializePushLock(&g_VidMm.CpuUpdateLock);
    if (!Device->FullWddm || !Device->VramEnabled || SegmentLength == 0) return STATUS_SUCCESS;
    if (((SegmentOffset|SegmentLength) & (PAGE_SIZE-1)) != 0 ||
        SegmentOffset > Device->VramLength || SegmentLength > Device->VramLength-SegmentOffset ||
        (ULONGLONG)(SIZE_T)SegmentLength != SegmentLength ||
        (ULONGLONG)Device->VramPhysical.QuadPart > MAXULONGLONG-SegmentOffset-SegmentLength)
        return STATUS_INVALID_PARAMETER;

    if (!TableLength || ((TableOffset|TableLength)&(PAGE_SIZE-1))!=0 ||
        TableOffset>Device->VramLength || TableLength>Device->VramLength-TableOffset ||
        (ULONGLONG)(SIZE_T)TableLength!=TableLength ||
        (ULONGLONG)Device->VramPhysical.QuadPart>MAXULONGLONG-TableOffset-TableLength ||
        VramSegmentId==0 || VramSegmentId>31 || TableSegmentId==0 || TableSegmentId>31)
        return STATUS_INVALID_PARAMETER;
    if (TableSegmentId==VramSegmentId) {
        if (TableOffset!=SegmentOffset || TableLength!=SegmentLength) return STATUS_INVALID_PARAMETER;
    } else if (TableOffset<SegmentOffset+SegmentLength && SegmentOffset<TableOffset+TableLength)
        return STATUS_INVALID_PARAMETER;
    // SegmentMapping covers table storage only. Application leaf addresses are
    // independently bounded by Pte.vram_base/vram_size below.
    g_VidMm.SegmentPhysical = (ULONGLONG)Device->VramPhysical.QuadPart + TableOffset;
    g_VidMm.SegmentLength = TableLength;
    // Settled by E18 run 001: PageAddress is a page frame number (a level 1 entry 0x1FD732 names the level 0 table at
    // segment offset 0x1FD732000), host memory is segment 0, level 0 is the leaf and level 3 the root.
    g_VidMm.Pte.units = BC250_PTE_ADDR_PAGES;
    g_VidMm.Pte.aperture = BC250_PTE_VM;
    g_VidMm.Pte.system_segment = 0;
    g_VidMm.Pte.vram_segment = VramSegmentId;
    // The walker wants physical addresses for VRAM pages and tables, not MC ones: amdgpu converts with
    // amdgpu_gmc_vram_mc2pa() (mc - vram_start + vram_base_offset) in gmc_v10_0_get_vm_pde()/get_vm_pte(), and
    // vram_base_offset is the FB offset register, which is what vram.c keeps as VramPhysical.
    g_VidMm.Pte.vram_base = (ULONGLONG)Device->VramPhysical.QuadPart + SegmentOffset;
    g_VidMm.Pte.vram_size = SegmentLength;
    if (TableSegmentId!=VramSegmentId) {
        g_VidMm.Pte.table_segment=TableSegmentId;
        g_VidMm.Pte.table_base=g_VidMm.SegmentPhysical;
        g_VidMm.Pte.table_size=TableLength;
    }
    g_VidMm.Pte.system_limit = VidMmSystemLimit();
    g_VidMm.Write = (GuardReadSetting(L"EnableGpuVa", 0) == 1) && Device->VramWriteEnabled;
    // Page-table update/walk paths must not allocate a mapping while building
    // DMA. Establish the bounded table extent once at start.
    if (g_VidMm.Write) {
        PHYSICAL_ADDRESS physical;
        unsigned tables=PagingPtShadowTableCount(BC250_VIDMM_PAGING_VA_BYTES,BC250_VIDMM_LEVELS);
        unsigned slots=tables*2u+1u; // <=50% occupancy; no growth during paging callbacks
        PAGING_PT_SHADOW_SLOT* storage;
        if (tables==0) return STATUS_INVALID_PARAMETER;
        storage=(PAGING_PT_SHADOW_SLOT*)ExAllocatePool2(POOL_FLAG_PAGED,
            (SIZE_T)slots*sizeof(*storage),BC250_VIDMM_SHADOW_TAG);
        if (storage==NULL) { g_VidMm.Write=FALSE; return STATUS_INSUFFICIENT_RESOURCES; }
        (void)PagingPtShadowInit(&g_VidMm.Shadow,storage,slots);
        GuardLog("vidmm: logical paging state %u tables, %u slots, %llu bytes reserved",tables,slots,
                 (ULONGLONG)((SIZE_T)slots*sizeof(*storage)));
        physical.QuadPart = (LONGLONG)g_VidMm.SegmentPhysical;
        g_VidMm.SegmentMapping = (PUCHAR)MmMapIoSpaceEx(physical,(SIZE_T)TableLength,PAGE_READWRITE|PAGE_NOCACHE);
        if (g_VidMm.SegmentMapping == NULL) {
            ExFreePoolWithTag(g_VidMm.Shadow.Slots,BC250_VIDMM_SHADOW_TAG);
            g_VidMm.Shadow.Slots=NULL;
            g_VidMm.Write = FALSE;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (g_VidMm.Write && Device->WddmAperture.bytes) {
        unsigned pages=(unsigned)(PAGING_APERTURE_BYTES/PAGE_SIZE);
        ULONGLONG* storage=(ULONGLONG*)ExAllocatePool2(POOL_FLAG_PAGED,
            (SIZE_T)pages*sizeof(ULONGLONG),BC250_VIDMM_SHADOW_TAG);
        if (!storage) return STATUS_INSUFFICIENT_RESOURCES;
        if (!PagingApertureStateInit(&g_VidMm.Aperture,&Device->WddmAperture,storage,pages)) {
            ExFreePoolWithTag(storage,BC250_VIDMM_SHADOW_TAG);return STATUS_INVALID_PARAMETER;
        }
    }
    g_VidMm.Ready = TRUE;
    GuardLog("vidmm: table segment %u at physical 0x%llX + 0x%llX, host memory below 0x%llX, updates are %s", TableSegmentId,
             g_VidMm.SegmentPhysical, TableLength, g_VidMm.Pte.system_limit, g_VidMm.Write ? "WRITTEN" : "planned only (EnableGpuVa closed)");
    return STATUS_SUCCESS;
}

NTSTATUS VidMmStart(_In_ const BC250_DEVICE* Device, ULONGLONG SegmentOffset, ULONGLONG SegmentLength, ULONG VramSegmentId)
{
    return VidMmStartLayout(Device,SegmentOffset,SegmentLength,VramSegmentId,
                           SegmentOffset,SegmentLength,VramSegmentId);
}

void VidMmStop(void)
{
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&g_VidMm.CpuUpdateLock);
    g_VidMm.Ready = FALSE;
    g_VidMm.Write = FALSE;
    if (g_VidMm.SegmentMapping != NULL) {
        MmUnmapIoSpace(g_VidMm.SegmentMapping,(SIZE_T)g_VidMm.SegmentLength);
        g_VidMm.SegmentMapping = NULL;
    }
    if (g_VidMm.Aperture.entries!=NULL) {
        ExFreePoolWithTag(g_VidMm.Aperture.entries,BC250_VIDMM_SHADOW_TAG);
        RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
    }
    if (g_VidMm.Shadow.Slots!=NULL) {
        ExFreePoolWithTag(g_VidMm.Shadow.Slots,BC250_VIDMM_SHADOW_TAG);
        RtlZeroMemory(&g_VidMm.Shadow,sizeof(g_VidMm.Shadow));
    }
    ExReleasePushLockExclusive(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
}

// Called only after DMA/private capacity has been accepted, before publication.
// Copy page identities under the same lock as other logical paging state. The
// MDL is OS-owned for this callback; no pointer is retained beyond it.
NTSTATUS VidMmCommitPagingAperture(const DXGKARG_BUILDPAGINGBUFFER* Build, ULONG Start, ULONG Next)
{
    ULONGLONG first,total,span,begin,pages[PAGING_APERTURE_BATCH_PAGES];
    ULONG count,i;
    PMDL mdl=NULL;
    BOOLEAN unmap;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    if (!Build || Next<=Start || Next-Start>PAGING_APERTURE_BATCH_PAGES) return status;
    count=Next-Start;unmap=Build->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT;
    if (unmap) {
        first=Build->UnmapApertureSegment.OffsetInPages;
        total=Build->UnmapApertureSegment.NumberOfPages;
    } else if (Build->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT) {
        first=Build->MapApertureSegment.OffsetInPages;
        total=Build->MapApertureSegment.NumberOfPages;
        mdl=Build->MapApertureSegment.pMdl;
        if (!mdl || !MmGetMdlByteCount(mdl)) return status;
        begin=MmGetMdlByteOffset(mdl);
        if (begin>=PAGE_SIZE) return status;
        span=(begin+(ULONGLONG)MmGetMdlByteCount(mdl)+PAGE_SIZE-1)>>PAGE_SHIFT;
        if (Build->MapApertureSegment.MdlOffset>span ||
            Next>span-Build->MapApertureSegment.MdlOffset) return status;
        for (i=0;i<count;i++) {
            ULONGLONG pfn=MmGetMdlPfnArray(mdl)[Build->MapApertureSegment.MdlOffset+Start+i];
            if (pfn>(MAXULONGLONG>>PAGE_SHIFT)) return status;
            pages[i]=pfn<<PAGE_SHIFT;
        }
    } else return status;
    if (Next>total || first>=PAGING_APERTURE_BYTES/PAGE_SIZE ||
        Next>PAGING_APERTURE_BYTES/PAGE_SIZE-first) return status;
    KeEnterCriticalRegion();ExAcquirePushLockExclusive(&g_VidMm.CpuUpdateLock);
    if (!g_VidMm.Ready || !g_VidMm.Write) status=STATUS_DEVICE_NOT_READY;
    else if (unmap ? PagingApertureStateUnmap(&g_VidMm.Aperture,(unsigned)(first+Start),count) :
             PagingApertureStateMap(&g_VidMm.Aperture,(unsigned)(first+Start),count,pages,~4095ull))
        status=STATUS_SUCCESS;
    ExReleasePushLockExclusive(&g_VidMm.CpuUpdateLock);KeLeaveCriticalRegion();
    return status;
}

BOOLEAN VidMmResolveAperture(ULONGLONG Mc, ULONG Bytes, ULONGLONG* Physical)
{
    BOOLEAN result;
    *Physical=0;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result=g_VidMm.Ready && g_VidMm.Write &&
        PagingApertureStateResolve(&g_VidMm.Aperture,Mc,Bytes,Physical);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);KeLeaveCriticalRegion();
    return result;
}

BOOLEAN VidMmApertureRangeValid(ULONGLONG Mc, ULONGLONG Bytes)
{
    ULONGLONG offset,physical;BOOLEAN result=FALSE;
    if (!Bytes || Mc>MAXULONGLONG-Bytes) return FALSE;
    KeEnterCriticalRegion();ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    if (!g_VidMm.Ready || !g_VidMm.Write || Mc<g_VidMm.Aperture.aperture.mc) goto Done;
    offset=Mc-g_VidMm.Aperture.aperture.mc;
    if (offset>=g_VidMm.Aperture.aperture.bytes || Bytes>g_VidMm.Aperture.aperture.bytes-offset) goto Done;
    while (Bytes) {
        ULONG count=PAGE_SIZE-(ULONG)(Mc&(PAGE_SIZE-1));
        if (count>Bytes) count=(ULONG)Bytes;
        if (!PagingApertureStateResolve(&g_VidMm.Aperture,Mc,count,&physical)) goto Done;
        Bytes-=count;Mc+=count;
    }
    result=TRUE;
Done:
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);KeLeaveCriticalRegion();
    return result;
}

static BOOLEAN VidMmTranslateRetainedViewLocked(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System, BOOLEAN Logical)
{
    const BC250_VIDMM* vm = &g_VidMm;
    ULONGLONG table = RootPhysical;
    int level;

    *Physical = 0;
    *System = FALSE;
    if (!vm->Ready || !vm->Write || (Logical ? vm->Shadow.Slots==NULL : vm->SegmentMapping==NULL) || vm->SegmentLength < PAGE_SIZE || RootPhysical == 0 ||
        (Va >> (PAGE_SHIFT + 9 * BC250_VIDMM_LEVELS)) != 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return FALSE;
    for (level = BC250_VIDMM_LEVELS - 1; level >= 0; level--)
    {
        struct bc250_pte_fields fields;
        volatile ULONGLONG* page;
        ULONGLONG entry;
        UINT index = (UINT)((Va >> (PAGE_SHIFT + 9 * level)) & (BC250_VIDMM_PTES - 1));

        if ((table & (PAGE_SIZE - 1)) != 0 || table < vm->SegmentPhysical || table > vm->SegmentPhysical + vm->SegmentLength - PAGE_SIZE)
            return FALSE;
        if (Logical) {
            if (PagingPtShadowRead(&vm->Shadow,table,index,&entry)!=PAGING_PT_OK) return FALSE;
        } else {
            page = (volatile ULONGLONG*)(vm->SegmentMapping+(SIZE_T)(table-vm->SegmentPhysical));
            entry = page[index];
        }
        bc250_pte_decode(entry, level == 0 ? BC250_PTE_LEAF : BC250_PTE_DIRECTORY, &fields);
        // A directory entry that is itself a page (PDE-as-PTE, a large mapping) is nothing this file writes; refuse
        // rather than misread it.
        if (!fields.valid || (level != 0 && fields.pde_pte)) return FALSE;
        table = fields.address;
        if (level == 0) *System = (fields.system != 0);
    }
    // Local leaf addresses may name either application or table storage.
    // Directory traversal above remains confined to the table extent.
    if (!*System && !VidMmLocalPageAllowed(vm,table))
        return FALSE;
    *Physical = table + (Va & (PAGE_SIZE - 1));
    return TRUE;
}

static BOOLEAN VidMmTranslateRetainedLocked(ULONGLONG RootPhysical, ULONGLONG Va,
                                      _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    return VidMmTranslateRetainedViewLocked(RootPhysical,Va,Physical,System,FALSE);
}

// Construction view includes accepted, possibly not-yet-executed PTE updates.
// A missing logical entry is a refusal, never a fallback to stale GPU memory.
BOOLEAN VidMmTranslateRetainedPaging(ULONGLONG RootPhysical, ULONGLONG Va,
                             _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    BOOLEAN result;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) { *Physical=0; *System=FALSE; return FALSE; }
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result=VidMmTranslateRetainedViewLocked(RootPhysical,Va,Physical,System,TRUE);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    return result;
}

BOOLEAN VidMmTranslateRetained(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Physical, _Out_ BOOLEAN* System)
{
    BOOLEAN result;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { *Physical=0; *System=FALSE; return FALSE; }
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result = VidMmTranslateRetainedLocked(RootPhysical,Va,Physical,System);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    return result;
}

static BOOLEAN VidMmProbeIbLocked(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Leaf, _Out_ ULONGLONG* Physical,
                     _Out_ BOOLEAN* System, _Out_writes_(BC250_IB_PROBE_DWORDS) ULONG* Dwords)
{
    const BC250_VIDMM* vm = &g_VidMm;
    ULONGLONG table = RootPhysical;
    ULONGLONG leaf = 0;
    ULONG i;
    int level;

    *Leaf = 0;
    *Physical = 0;
    *System = FALSE;
    for (i = 0; i < BC250_IB_PROBE_DWORDS; i++) Dwords[i] = 0;
    if (!vm->Ready || !vm->Write || vm->SegmentMapping==NULL || vm->SegmentLength < PAGE_SIZE || RootPhysical == 0 ||
        (Va >> (PAGE_SHIFT + 9 * BC250_VIDMM_LEVELS)) != 0 || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return FALSE;
    for (level = BC250_VIDMM_LEVELS - 1; level >= 0; level--)
    {
        struct bc250_pte_fields fields;
        volatile ULONGLONG* page;
        ULONGLONG entry;
        UINT index = (UINT)((Va >> (PAGE_SHIFT + 9 * level)) & (BC250_VIDMM_PTES - 1));

        if ((table & (PAGE_SIZE - 1)) != 0 || table < vm->SegmentPhysical ||
            table > vm->SegmentPhysical + vm->SegmentLength - PAGE_SIZE)
            return FALSE;
        page = (volatile ULONGLONG*)(vm->SegmentMapping+(SIZE_T)(table-vm->SegmentPhysical));
        entry = page[index];
        if (level == 0) leaf = entry;
        bc250_pte_decode(entry, level == 0 ? BC250_PTE_LEAF : BC250_PTE_DIRECTORY, &fields);
        if (!fields.valid || (level != 0 && fields.pde_pte)) return FALSE;
        table = fields.address;
        if (level == 0) *System = (fields.system != 0);
    }
    *Leaf = leaf;
    if (!*System && !VidMmLocalPageAllowed(vm,table))
        return FALSE;
    *Physical = table + (Va & (PAGE_SIZE - 1));
    if (*System) {
        MM_COPY_ADDRESS source;
        SIZE_T copied=0,bytes;
        ULONG off=(ULONG)(Va & (PAGE_SIZE-1));
        NTSTATUS status;
        if (!vm->Pte.system_limit || table>=vm->Pte.system_limit ||
            PAGE_SIZE>vm->Pte.system_limit-table || (off&3u)!=0 ||
            (table & (PAGE_SIZE-1))!=0 || VidMmLocalPageAllowed(vm,table)) return FALSE;
        bytes=PAGE_SIZE-off;
        if (bytes>BC250_IB_PROBE_DWORDS*sizeof(ULONG)) bytes=BC250_IB_PROBE_DWORDS*sizeof(ULONG);
        source.PhysicalAddress.QuadPart=(LONGLONG)(table+off);
        // Regular OS RAM only. Do not create an NC alias of an allocation whose
        // existing cache attributes are OS-owned. Caller supplies nonpaged output.
        status=MmCopyMemory(Dwords,source,bytes,MM_COPY_MEMORY_PHYSICAL,&copied);
        if (!NT_SUCCESS(status) || copied!=bytes) return FALSE;
    }
    return TRUE;
}

// CPU table-map lifetime and PTE CPU writers share the same lock as other walks.
// This is a diagnostic snapshot, not GPU synchronization or proof of pinning.
BOOLEAN VidMmProbeIb(ULONGLONG RootPhysical, ULONGLONG Va, _Out_ ULONGLONG* Leaf, _Out_ ULONGLONG* Physical,
                     _Out_ BOOLEAN* System, _Out_writes_(BC250_IB_PROBE_DWORDS) ULONG* Dwords)
{
    BOOLEAN result;
    *Leaf=0;*Physical=0;*System=FALSE;
    RtlZeroMemory(Dwords,BC250_IB_PROBE_DWORDS*sizeof(ULONG));
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return FALSE;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&g_VidMm.CpuUpdateLock);
    result=VidMmProbeIbLocked(RootPhysical,Va,Leaf,Physical,System,Dwords);
    ExReleasePushLockShared(&g_VidMm.CpuUpdateLock);
    KeLeaveCriticalRegion();
    if (!result) {
        *Leaf=0;*Physical=0;*System=FALSE;
        RtlZeroMemory(Dwords,BC250_IB_PROBE_DWORDS*sizeof(ULONG));
    }
    return result;
}

NTSTATUS GartCaptureAperture(BC250_DEVICE* Device, PAGING_APERTURE* Aperture)
{
    struct amdgpu_device* adev=NULL;
    BOOLEAN enabled=FALSE;
    NTSTATUS status;
    if (!Aperture) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Aperture,sizeof(*Aperture));
    if (!Device) return STATUS_INVALID_PARAMETER;
    ExAcquireFastMutex(&Device->GartLock);
    status=GartDevice(Device,&adev,&enabled);
    if (NT_SUCCESS(status)) {
        if (!adev || !adev->gart.bo ||
            !PagingApertureInit(adev->gmc.gart_start,adev->gmc.gart_size,
                adev->gart.bo->gpu_addr,adev->gart.table_size,Aperture))
            status=STATUS_DEVICE_NOT_READY;
    }
    ExReleaseFastMutex(&Device->GartLock);
    return status;
}

static BOOLEAN WddmMemoryLayout(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset, _Out_ ULONGLONG* Length,
                                _Out_ ULONGLONG* TableOffset, _Out_ ULONGLONG* TableLength)
{
    ULONGLONG fbOffset,framebufferEnd=0,available,limit,tableBytes;
    *Offset=*Length=*TableOffset=*TableLength=0;
    if (!Device->VramEnabled || Device->VramLength<=BC250_VRAM_TOP_RESERVED) return FALSE;
    limit=Device->VramLength-BC250_VRAM_TOP_RESERVED;
    if (VramFramebufferOffset(Device,&fbOffset)) {
        ULONGLONG bytes=(ULONGLONG)Device->Post.Pitch*Device->Post.Height;
        if (fbOffset>limit || bytes>limit-fbOffset) return FALSE;
        framebufferEnd=fbOffset+bytes;
    }
    if (framebufferEnd>MAXULONGLONG-65535ull) return FALSE;
    framebufferEnd=(framebufferEnd+65535ull)&~65535ull;
    if (framebufferEnd>=limit) return FALSE;
    available=(limit-framebufferEnd)&~65535ull;
    // Capacity policy: reserve 1/32 of usable VRAM, rounded down to64KiB,
    // at least4MiB for the pinned1GiB paging hierarchy plus application tables.
    // This is a driver budget choice, not a hardware limit or performance claim.
    tableBytes=(available/32u)&~65535ull;
    if (tableBytes<4ull*1024*1024) tableBytes=4ull*1024*1024;
    if (tableBytes>=available || available-tableBytes<65536ull) return FALSE;
    *Offset=framebufferEnd;*Length=available-tableBytes;
    *TableOffset=*Offset+*Length;*TableLength=tableBytes;
    return TRUE;
}

// SegmentAddress already includes the advertised MC base. Offset is an
// allocation-relative byte offset (TransferOffset), never another segment base.
static BOOLEAN WddmLocalPagingEndpoint(const BC250_DEVICE* Device, UINT Segment,
    ULONGLONG SegmentAddress, ULONGLONG Offset, ULONGLONG Bytes, BC250_PAGING_ENDPOINT* Endpoint)
{
    ULONGLONG appOffset,appLength,tableOffset,tableLength,base,length,address;
    RtlZeroMemory(Endpoint,sizeof(*Endpoint));
    if (!Bytes) return FALSE;
    if (Segment==BC250_WDDM_SEGMENT_APERTURE) {
        base=Device->WddmAperture.mc;length=Device->WddmAperture.bytes;
        if (!length || SegmentAddress<base || SegmentAddress-base>=length ||
            Offset>MAXULONGLONG-SegmentAddress) return FALSE;
        address=SegmentAddress+Offset;
        if (address-base>=length || Bytes>length-(address-base)) return FALSE;
        Endpoint->Address=address;Endpoint->Length=length-(address-base);Endpoint->Aperture=TRUE;
        return TRUE;
    }
    if (!WddmMemoryLayout(Device,&appOffset,&appLength,&tableOffset,&tableLength)) return FALSE;
    if (Segment==BC250_WDDM_SEGMENT_VRAM) { base=appOffset;length=appLength; }
    else if (Segment==BC250_WDDM_SEGMENT_TABLES) { base=tableOffset;length=tableLength; }
    else return FALSE; // Unknown segment is not a local-memory endpoint.
    if (base>MAXULONGLONG-Device->VramMcBase) return FALSE;
    base+=Device->VramMcBase;
    if (SegmentAddress<base || SegmentAddress-base>=length ||
        Offset>MAXULONGLONG-SegmentAddress) return FALSE;
    address=SegmentAddress+Offset;
    if (address-base>=length || Bytes>length-(address-base) || Bytes>MAXULONGLONG-address) return FALSE;
    Endpoint->Address=address;Endpoint->Length=length-(address-base);
    return TRUE;
}

// Preparation only: no packets, mappings or ownership changes. Start/End flags
// describe OS sub-transfers; they do not reset MultipassOffset on repeated calls.
BOOLEAN WddmPreparePhysicalTransfer(const BC250_DEVICE* Device,
    const DXGKARG_BUILDPAGINGBUFFER* Build, BC250_PAGING_ENDPOINT* Source,
    BC250_PAGING_ENDPOINT* Destination, ULONGLONG* Progress)
{
    BC250_PAGING_ENDPOINT source,destination;
    ULONGLONG bytes,progress;
    if (!Source || !Destination || !Progress) return FALSE;
    RtlZeroMemory(Source,sizeof(*Source));RtlZeroMemory(Destination,sizeof(*Destination));*Progress=0;
    if (!Device || !Build || Source==Destination || !Build->Transfer.TransferSize ||
        Build->Transfer.Flags.Swizzle || Build->Transfer.Flags.Unswizzle || Build->Transfer.Flags.Reserved)
        return FALSE;
    bytes=Build->Transfer.TransferSize;
    RtlZeroMemory(&source,sizeof(source));RtlZeroMemory(&destination,sizeof(destination));
    if (Build->Transfer.Source.SegmentId==0) {
        source.Mdl=Build->Transfer.Source.pMdl;source.FirstPage=Build->Transfer.MdlOffset;source.Length=bytes;
        if (!source.Mdl) return FALSE;
    } else if (!WddmLocalPagingEndpoint(Device,Build->Transfer.Source.SegmentId,
        (ULONGLONG)Build->Transfer.Source.SegmentAddress.QuadPart,Build->Transfer.TransferOffset,bytes,&source))
        return FALSE;
    if (Build->Transfer.Destination.SegmentId==0) {
        destination.Mdl=Build->Transfer.Destination.pMdl;
        destination.FirstPage=Build->Transfer.MdlOffset;destination.Length=bytes;
        if (!destination.Mdl) return FALSE;
    } else if (!WddmLocalPagingEndpoint(Device,Build->Transfer.Destination.SegmentId,
        (ULONGLONG)Build->Transfer.Destination.SegmentAddress.QuadPart,Build->Transfer.TransferOffset,bytes,&destination))
        return FALSE;
    if (!GfxPagingEndpointValid(Device,&source,bytes) || !GfxPagingEndpointValid(Device,&destination,bytes) ||
        !PagingStreamTokenDecode(FALSE,source.Address,destination.Address,bytes,Build->MultipassOffset,&progress))
        return FALSE;
    // Whole-range logical preflight once per DDI, not for every copy slice.
    if ((source.Aperture && !VidMmApertureRangeValid(source.Address,bytes)) ||
        (destination.Aperture && !VidMmApertureRangeValid(destination.Address,bytes))) return FALSE;
    *Source=source;*Destination=destination;*Progress=progress;
    return TRUE;
}

static NTSTATUS WddmPublishPagingRecordCore(_Inout_ DXGKARG_BUILDPAGINGBUFFER* Build,
                                        ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next,
                                        ULONGLONG CopySource, ULONGLONG CopyDestination, ULONG CopyCount,
                                        ULONGLONG FillPhysical, ULONGLONG FillBytes, ULONG FillPattern,
                                        const BC250_PAGING_COPY_SLICE* Transfer)
{
    ULONG* record;
    NTSTATUS status;
    if (Build==NULL || Build->pDmaBuffer==NULL || Build->pDmaBufferPrivateData==NULL ||
        Written==0 || Written>Build->DmaSize/sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
    record=(ULONG*)Build->pDmaBufferPrivateData;
    if (!PagingPrivateHeader((unsigned*)record,Build->DmaBufferPrivateDataSize,
            Build->DmaBufferWriteOffset,Build->DmaBufferGpuVirtualAddress,Written*4u)) return STATUS_INVALID_PARAMETER;
    if (Update) {
        if (Next<=Start) { record[0]=0; return STATUS_INVALID_PARAMETER; }
        status=VidMmCommitPagingUpdate(&Build->UpdatePageTable,Start,Next-Start);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (CopyCount!=0) {
        status=VidMmCommitPagingCopy(CopySource,CopyDestination,CopyCount);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (FillBytes) {
        status=VidMmCommitPagingFill(FillPhysical,FillBytes,FillPattern);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (Transfer) {
        status=VidMmCommitPagingTransfer(Transfer);
        if (!NT_SUCCESS(status)) { record[0]=0;return status; }
    }
    if (Build->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
        Build->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        status=VidMmCommitPagingAperture(Build,Start,Next);
        if (!NT_SUCCESS(status)) {record[0]=0;return status;}
    }
    RtlCopyMemory(Build->pDmaBuffer,(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Written*4u);
    Build->pDmaBufferPrivateData=(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES+Written*4u;
    Build->DmaBufferPrivateDataSize-=PAGING_PRIVATE_HEADER_BYTES+Written*4u;
    if (Build->DmaBufferPrivateDataSize>=sizeof(ULONG)) *(ULONG*)Build->pDmaBufferPrivateData=0;
    Build->pDmaBuffer=(PUCHAR)Build->pDmaBuffer+Written*4u;
    Build->DmaSize-=Written*4u;
    return STATUS_SUCCESS;
}

static NTSTATUS WddmPublishPagingRecordFull(DXGKARG_BUILDPAGINGBUFFER* Build,
    ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next,
    ULONGLONG CopySource, ULONGLONG CopyDestination, ULONG CopyCount,
    ULONGLONG FillPhysical, ULONGLONG FillBytes, ULONG FillPattern)
{
    return WddmPublishPagingRecordCore(Build,Written,Update,Start,Next,CopySource,CopyDestination,
        CopyCount,FillPhysical,FillBytes,FillPattern,NULL);
}

static NTSTATUS WddmPublishPagingRecordEx(_Inout_ DXGKARG_BUILDPAGINGBUFFER* Build,
                                        ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next,
                                        ULONGLONG CopySource, ULONGLONG CopyDestination, ULONG CopyCount)
{
    return WddmPublishPagingRecordFull(Build,Written,Update,Start,Next,
        CopySource,CopyDestination,CopyCount,0,0,0);
}

static NTSTATUS WddmPublishPagingRecord(_Inout_ DXGKARG_BUILDPAGINGBUFFER* Build,
                                        ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next)
{
    return WddmPublishPagingRecordEx(Build,Written,Update,Start,Next,0,0,0);
}

// MultipassOffset counts whole ranges, each at most one advertised 4 KiB table.
// Publish each accepted range before resolving the next: later ranges may depend
// on earlier logical copies, even before the GPU executes the buffer.
static NTSTATUS WddmBuildPagingCopies(_Inout_ BC250_DEVICE* Device, ULONGLONG Root,
                                     _Inout_ DXGKARG_BUILDPAGINGBUFFER* Build)
{
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    NTSTATUS status=STATUS_SUCCESS;
    if (Build->MultipassOffset>Build->CopyPageTableEntries.NumRanges ||
        (Build->CopyPageTableEntries.NumRanges && !Build->CopyPageTableEntries.pRanges))
        return STATUS_INVALID_PARAMETER;
    while (Build->MultipassOffset<Build->CopyPageTableEntries.NumRanges) {
        const DXGK_BUILDPAGINGBUFFER_COPY_RANGE* range=
            &Build->CopyPageTableEntries.pRanges[Build->MultipassOffset];
        ULONG written=0,freeBytes=Build->DmaSize;
        ULONGLONG source=0,destination=0;
        BC250_WDDM_PAGING_UNSUPPORTED unsupported;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (freeBytes>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
            freeBytes=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
        status=GfxPagingBuildCopyRange(Device,Root,range,(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,
            Build->DmaBufferWriteOffset,freeBytes,&written,&source,&destination,&unsupported);
        if (!NT_SUCCESS(status)) break;
        status=WddmPublishPagingRecordEx(Build,written,FALSE,0,0,
            source,destination,range->NumPageTableEntries);
        if (!NT_SUCCESS(status)) break;
        Build->MultipassOffset++;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    // The OS owns the input offset. Only pointers, remaining sizes and progress
    // are outputs; the local offset above accounts for every range in this call.
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

// UINT token counts page slices, not bytes. The first/last slices may be
// partial; this preserves unaligned DWORD fills and progress beyond 4 GiB.
static NTSTATUS WddmBuildPhysicalFill(BC250_DEVICE* Device, DXGKARG_BUILDPAGINGBUFFER* Build,
                                     ULONGLONG* Moved)
{
    BC250_PAGING_ENDPOINT destination;
    ULONGLONG bytes=Build->Fill.FillSize,start,next;
    ULONG token=Build->MultipassOffset,written=0,capacity;
    unsigned nextToken;
    ULONG* record;
    NTSTATUS status;
    *Moved=0;
    if (!WddmLocalPagingEndpoint(Device,Build->Fill.Destination.SegmentId,
        (ULONGLONG)Build->Fill.Destination.SegmentAddress.QuadPart,0,bytes,&destination) ||
        ((destination.Address | bytes)&3)!=0 || !Build->pDmaBuffer) return STATUS_INVALID_PARAMETER;
    if (destination.Aperture && !VidMmApertureRangeValid(destination.Address,bytes)) return STATUS_INVALID_PARAMETER;
    if (!PagingStreamTokenDecode(TRUE,0,destination.Address,bytes,token,&start))
        return STATUS_INVALID_PARAMETER;
    if (start==bytes) return STATUS_SUCCESS;
    if (!Build->pDmaBufferPrivateData || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    record=(ULONG*)Build->pDmaBufferPrivateData;record[0]=0;
    capacity=Build->DmaSize;
    if (capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
        capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
    status=GfxPagingBuildPhysical(Device,NULL,&destination,TRUE,bytes,Build->Fill.FillPattern,
        (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,
        start,&written,&next);
    if (status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) return status;
    if (!PagingStreamTokenEncode(TRUE,0,destination.Address,bytes,next,&nextToken))
        return STATUS_INVALID_PARAMETER;
    if (written) {
        ULONGLONG physical=0,fillBytes=0;
        NTSTATUS publish;
        if (Build->Fill.Destination.SegmentId==BC250_WDDM_SEGMENT_TABLES) {
            ULONGLONG offset=destination.Address-Device->VramMcBase+start;
            physical=(ULONGLONG)Device->VramPhysical.QuadPart;
            if (offset>MAXULONGLONG-physical) return STATUS_INVALID_PARAMETER;
            physical+=offset;fillBytes=next-start;
        }
        publish=WddmPublishPagingRecordFull(Build,written,FALSE,0,0,0,0,0,
            physical,fillBytes,Build->Fill.FillPattern);
        if (!NT_SUCCESS(publish)) return publish;
    }
    Build->MultipassOffset=nextToken;
    *Moved=next-start;
    return status;
}

static NTSTATUS WddmBuildVirtualFill(BC250_DEVICE* Device, ULONGLONG Root,
    DXGKARG_BUILDPAGINGBUFFER* Build, ULONGLONG* Moved)
{
    ULONGLONG bytes=Build->FillVirtual.FillSizeInBytes,va=Build->FillVirtual.DestinationVirtualAddress;
    ULONGLONG app,appLength,table,tableLength,tablePhysical,progress;
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    // Slice tokens preserve full64-bit byte progress without per-request storage.
    if (!Root || !Build->pDmaBuffer || !PagingStreamTokenDecode(TRUE,0,va,bytes,Build->MultipassOffset,&progress) ||
        !WddmMemoryLayout(Device,&app,&appLength,&table,&tableLength) ||
        table>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_INVALID_PARAMETER;
    tablePhysical=(ULONGLONG)Device->VramPhysical.QuadPart+table;
    while (progress<bytes) {
        ULONGLONG address=va+progress,physical;
        ULONG count=(ULONG)(PAGE_SIZE-(address&(PAGE_SIZE-1))),written=0,capacity=Build->DmaSize;
        unsigned nextToken;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        BOOLEAN system;
        if (count>bytes-progress) count=(ULONG)(bytes-progress);
        if (!PagingStreamTokenEncode(TRUE,0,va,bytes,progress+count,&nextToken)) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
            capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
        status=GfxPagingBuildFillPage(Device,Root,address,count,Build->FillVirtual.FillPattern,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,
            &written,&physical,&system);
        if (!NT_SUCCESS(status)) break;
        // Match the captured physical identity, never translate this VA again
        // after committing an earlier slice that could modify its mapping.
        {
            ULONGLONG fillBytes=0;
            if (!system && physical>=tablePhysical && physical-tablePhysical<tableLength) {
                if (count>tableLength-(physical-tablePhysical)) {status=STATUS_INVALID_PARAMETER;break;}
                fillBytes=count;
            }
            status=WddmPublishPagingRecordFull(Build,written,FALSE,0,0,0,0,0,
                physical,fillBytes,Build->FillVirtual.FillPattern);
        }
        if (!NT_SUCCESS(status)) break;
        progress+=count;Build->MultipassOffset=nextToken;*Moved+=count;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

// Both endpoints use the paging process root: SourcePageTableVaInTransfer is
// not advertised. Commit each accepted slice before translating the next one.
static NTSTATUS WddmBuildVirtualTransfer(BC250_DEVICE* Device, ULONGLONG Root,
    DXGKARG_BUILDPAGINGBUFFER* Build, ULONGLONG* Moved)
{
    ULONGLONG bytes=Build->TransferVirtual.TransferSizeInBytes;
    ULONGLONG src=Build->TransferVirtual.SourceVirtualAddress,dst=Build->TransferVirtual.DestinationVirtualAddress;
    ULONGLONG app,appLength,table,tableLength,tablePhysical,progress;
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    if (!Root || !Build->pDmaBuffer || !PagingStreamTokenDecode(FALSE,src,dst,bytes,Build->MultipassOffset,&progress) ||
        !WddmMemoryLayout(Device,&app,&appLength,&table,&tableLength) ||
        table>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_INVALID_PARAMETER;
    tablePhysical=(ULONGLONG)Device->VramPhysical.QuadPart+table;
    while (progress<bytes) {
        ULONG count=(ULONG)(PAGE_SIZE-((src+progress)&(PAGE_SIZE-1)));
        ULONG destinationRoom=(ULONG)(PAGE_SIZE-((dst+progress)&(PAGE_SIZE-1)));
        ULONG written=0,capacity=Build->DmaSize;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        BC250_PAGING_COPY_SLICE slice;
        const BC250_PAGING_COPY_SLICE* commit=NULL;
        unsigned nextToken;
        if (count>destinationRoom) count=destinationRoom;
        if (count>bytes-progress) count=(ULONG)(bytes-progress);
        if (!PagingStreamTokenEncode(FALSE,src,dst,bytes,progress+count,&nextToken)) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
            capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
        // Per-slice aliases stage. Arbitrary cross-page physical alias dependencies
        // are still unresolved; VA ordering alone cannot establish physical order.
        status=GfxPagingBuildVirtualCopyPage(Device,Root,src+progress,dst+progress,count,FALSE,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,&written,&slice);
        if (!NT_SUCCESS(status)) break;
        if (!slice.DestinationSystem && slice.DestinationPhysical>=tablePhysical &&
            slice.DestinationPhysical-tablePhysical<tableLength) {
            if (count>tableLength-(slice.DestinationPhysical-tablePhysical)) {
                status=STATUS_INVALID_PARAMETER;break;
            }
            commit=&slice;
        }
        status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,commit);
        if (!NT_SUCCESS(status)) break;
        progress+=count;Build->MultipassOffset=nextToken;*Moved+=count;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

static NTSTATUS WddmBuildAperture(BC250_DEVICE* Device, DXGKARG_BUILDPAGINGBUFFER* Build, BOOLEAN Unmap)
{
    BC250_PAGING_APERTURE_OP operation;
    ULONG written=0,next=Build->MultipassOffset,capacity=Build->DmaSize;
    ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
    NTSTATUS status,publish;
    RtlZeroMemory(&operation,sizeof(operation));operation.Unmap=Unmap;
    if (Unmap) {
        if (Build->UnmapApertureSegment.SegmentId!=BC250_WDDM_SEGMENT_APERTURE) return STATUS_INVALID_PARAMETER;
        operation.FirstPage=Build->UnmapApertureSegment.OffsetInPages;
        operation.PageCount=Build->UnmapApertureSegment.NumberOfPages;
        operation.DummyPhysical=(ULONGLONG)Build->UnmapApertureSegment.DummyPage.QuadPart;
    } else {
        if (Build->MapApertureSegment.SegmentId!=BC250_WDDM_SEGMENT_APERTURE ||
            Build->MapApertureSegment.Flags.Reserved) return STATUS_INVALID_PARAMETER;
        operation.FirstPage=Build->MapApertureSegment.OffsetInPages;
        operation.PageCount=Build->MapApertureSegment.NumberOfPages;
        operation.Mdl=Build->MapApertureSegment.pMdl;operation.MdlOffset=Build->MapApertureSegment.MdlOffset;
        operation.CacheCoherent=(BOOLEAN)(Build->MapApertureSegment.Flags.CacheCoherent!=0);
    }
    if (!Build->pDmaBuffer) return STATUS_INVALID_PARAMETER;
    if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    record[0]=0;
    if (capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
        capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
    status=GfxPagingBuildAperture(Device,&operation,Build->MultipassOffset,
        (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,&written,&next);
    if (written) {
        publish=WddmPublishPagingRecord(Build,written,FALSE,Build->MultipassOffset,next);
        if (!NT_SUCCESS(publish)) return publish;
        Build->MultipassOffset=next;
    }
    return status;
}

static NTSTATUS WddmBuildPhysicalTransfer(BC250_DEVICE* Device, DXGKARG_BUILDPAGINGBUFFER* Build,
                                         ULONGLONG* Moved)
{
    BC250_PAGING_ENDPOINT source,destination;
    ULONGLONG progress,total=Build->Transfer.TransferSize;
    unsigned count;
    DXGKARG_BUILDPAGINGBUFFER prepare=*Build;
    BOOLEAN permutationResume=(BOOLEAN)((Build->MultipassOffset&PAGING_PERMUTATION_RESUME)!=0);
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    BOOLEAN overlap=FALSE,reverse=FALSE;
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    if(permutationResume)prepare.MultipassOffset=0;
    if (!Build->pDmaBuffer || !WddmPreparePhysicalTransfer(Device,&prepare,&source,&destination,&progress) ||
        !PagingStreamTokenEncode(FALSE,source.Address,destination.Address,total,total,&count))
        return STATUS_INVALID_PARAMETER;
    // Indirect physical page lists can contain arbitrary cross-page alias cycles.
    // Whole-page bijections use complete cycles or bounded pivot swaps.
    // Partial-page and non-bijective alias graphs remain separate work.
    if ((source.Mdl || source.Aperture) && (destination.Mdl || destination.Aperture) &&
        (source.Aperture || destination.Aperture || source.Mdl!=destination.Mdl) && total>PAGE_SIZE) {
        BOOLEAN disjoint=FALSE;
        status=GfxPagingCheckDisjoint(Device,&source,&destination,total,&disjoint);
        if (!NT_SUCCESS(status)) return status;
        if (!disjoint) {
            ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
            ULONG capacity=Build->DmaSize,written=0;
            unsigned next=Build->MultipassOffset;
            NTSTATUS publish;
            // The tagged token identifies the next atomic group in the plan. No
            // scratch value is retained across BuildPagingBuffer calls.
            if (progress==total) return STATUS_SUCCESS;
            if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
                return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            if (capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
                capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
            record[0]=0;
            status=GfxPagingBuildPermutation(Device,&source,&destination,total,
                (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,originalOffset,capacity,Build->MultipassOffset,&written,&next);
            // Keep the existing refusal policy for unimplemented alias graphs;
            // the restricted DDI error contract remains tracked in the audit.
            if (status==STATUS_NOT_SUPPORTED) return STATUS_INVALID_PARAMETER;
            if (status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) return status;
            if (written) {
                publish=WddmPublishPagingRecord(Build,written,FALSE,0,0);
                if (!NT_SUCCESS(publish)) return publish;
            }
            // Count logical transfer bytes once its entire command plan has
            // been built. Intermediate swaps are not finalized source pages.
            *Moved=status==STATUS_SUCCESS?total:0;
            Build->MultipassOffset=next;
            return status;
        }
    }
    if (permutationResume) return STATUS_INVALID_PARAMETER;
    if (!source.Mdl && !destination.Mdl && !source.Aperture && !destination.Aperture) {
        overlap=source.Address<=destination.Address ? destination.Address-source.Address<total :
            source.Address-destination.Address<total;
        reverse=overlap && destination.Address>source.Address;
    }
    while (Build->MultipassOffset<count) {
        unsigned index=reverse ? count-1-Build->MultipassOffset : Build->MultipassOffset;
        ULONGLONG begin,end;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        ULONG capacity=Build->DmaSize,written=0;
        BC250_PAGING_COPY_SLICE slice;
        if (!PagingStreamTokenDecode(FALSE,source.Address,destination.Address,total,index,&begin) ||
            !PagingStreamTokenDecode(FALSE,source.Address,destination.Address,total,index+1,&end) ||
            end<=begin || end-begin>PAGE_SIZE) {status=STATUS_INVALID_PARAMETER;break;}
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (capacity>Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES)
            capacity=Build->DmaBufferPrivateDataSize-PAGING_PRIVATE_HEADER_BYTES;
        status=GfxPagingBuildCopyPageEx(Device,&source,&destination,begin,(ULONG)(end-begin),overlap,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,&written,&slice);
        if (!NT_SUCCESS(status)) break;
        status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,
            Build->Transfer.Destination.SegmentId==BC250_WDDM_SEGMENT_TABLES ? &slice : NULL);
        if (!NT_SUCCESS(status)) break;
        Build->MultipassOffset++;*Moved+=slice.Bytes;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

static NTSTATUS WddmQuerySegment4(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_QUERYSEGMENTOUT4* out = (DXGK_QUERYSEGMENTOUT4*)Query->pOutputData;
    DXGK_SEGMENTDESCRIPTOR4* descriptor;
    ULONGLONG offset, length, tableOffset, tableLength;
    UINT count;

    if (Query->OutputDataSize < sizeof(*out) || out == NULL) return STATUS_BUFFER_TOO_SMALL;
    count = WddmMemoryLayout(Device, &offset, &length, &tableOffset, &tableLength) ? 1u : 0u;

    // Geometry is captured before publishing WDDM state; queries never run setup.
    if (count != 0 && Device->WddmAperture.bytes!=PAGING_APERTURE_BYTES) {
        g_ApertureOffered=FALSE;
        return STATUS_DEVICE_NOT_READY;
    }
    if (count != 0) count = 3;          // application local, aperture, table local
    g_ApertureOffered = (count == 3);
    if (out->NbSegment == 0)
    {
        out->NbSegment = count;
        if (WddmAnswersLogged(Device)) GuardLog("wddm: QUERYSEGMENT4 pass 1: %u segment(s)", count);
        return STATUS_SUCCESS;
    }
    if (out->NbSegment < count || out->pSegmentDescriptor == NULL) return STATUS_INVALID_PARAMETER;
    if (count != 0)
    {
        if (out->SegmentDescriptorStride < sizeof(*descriptor)) return STATUS_INVALID_PARAMETER;
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)out->pSegmentDescriptor;
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->Flags.CpuVisible = 1;               // the carve-out is reachable by physical address (M31)
        descriptor->Flags.LocalBudgetGroup = 1;         // local memory, not an aperture: Aperture and Agp stay 0
        descriptor->Flags.DirectFlip = 1;               // the cap says DirectFlip; this is the segment scanned out
        // PreservedDuringStandby and PreservedDuringHibernate stay 0: whether this memory survives S3 on this
        // board is not known, and claiming it wrongly would hand back corrupt surfaces after a resume.
        descriptor->BaseAddress.QuadPart = (LONGLONG)(Device->VramMcBase + offset);
        descriptor->CpuTranslatedAddress.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)offset;
        descriptor->Size = (SIZE_T)length;
        // CommitLimit is left 0: the header says it applies to aperture segments only, and this is a memory
        // segment. Everything else in the descriptor - the VPR range, the invalid ranges - is zero for the same
        // reason: stage A has none of it.

        // The permanent OS aperture excludes private GTT and temporary paging
        // slots. CPU mapping policy is unchanged and remains an audited gap.
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)out->pSegmentDescriptor + out->SegmentDescriptorStride);
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->Flags.Aperture = 1;
        descriptor->Flags.CacheCoherent = 1;
        descriptor->Flags.CpuVisible = 1;
        descriptor->BaseAddress.QuadPart = (LONGLONG)Device->WddmAperture.mc;
        descriptor->CpuTranslatedAddress.QuadPart = (LONGLONG)0xFFFFFFFE00000000ull;
        descriptor->Size = (SIZE_T)Device->WddmAperture.bytes;
        descriptor->CommitLimit = (SIZE_T)Device->WddmAperture.bytes;

        // Table storage is excluded from every application allocation mask.
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)out->pSegmentDescriptor + 2u*out->SegmentDescriptorStride);
        RtlZeroMemory(descriptor,sizeof(*descriptor));
        descriptor->Flags.CpuVisible=1;
        descriptor->Flags.LocalBudgetGroup=1;
        descriptor->BaseAddress.QuadPart=(LONGLONG)(Device->VramMcBase+tableOffset);
        descriptor->CpuTranslatedAddress.QuadPart=Device->VramPhysical.QuadPart+(LONGLONG)tableOffset;
        descriptor->Size=(SIZE_T)tableLength;

    }
    out->NbSegment = count;
    // 0 is "system memory": VidMm then allocates the paging buffer itself, contiguous and write-combined. The
    // documented contract is "an aperture segment or 0", and segment 1 is local memory with Aperture clear; up to
    // 0.7.4 this said 1, and E16 run 004 was torn down right after CreateContext, before any root page table call
    // (facts M65, M66). A real aperture segment needs a working GART and belongs to stage B.
    out->PagingBufferSegmentId = 0;
    out->PagingBufferSize = BC250_WDDM_PAGING_BUFFER_BYTES;
    out->PagingBufferPrivateDataSize = PAGING_PRIVATE_BUFFER_BYTES;
    // The segment table is the centre of two suspects of E16 run 1 and was invisible in the log.
    if (!WddmAnswersLogged(Device)) return STATUS_SUCCESS;
    GuardLog("wddm: QUERYSEGMENT4 pass 2: %u segment(s), stride %u, paging buffer segment %u, %u bytes",
             count, (ULONG)out->SegmentDescriptorStride, out->PagingBufferSegmentId, out->PagingBufferSize);
    if (count != 0)
    {
        // CpuTranslatedAddress shares a union with CpuHostAperture and is the valid arm only while
        // Flags.SupportsCpuHostAperture is 0, which it is here.
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)out->pSegmentDescriptor;
        GuardLog("wddm: segment 1 flags 0x%08X gpu 0x%llX cpu 0x%llX size 0x%llX", descriptor->Flags.Value,
                 (ULONGLONG)descriptor->BaseAddress.QuadPart, (ULONGLONG)descriptor->CpuTranslatedAddress.QuadPart,
                 (ULONGLONG)descriptor->Size);
    }
    return STATUS_SUCCESS;
}
static NTSTATUS WddmPageTableLevelDesc(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    const DXGK_QUERYPAGETABLELEVELDESCIN* in = (const DXGK_QUERYPAGETABLELEVELDESCIN*)Query->pInputData;
    DXGK_PAGE_TABLE_LEVEL_DESC* desc = (DXGK_PAGE_TABLE_LEVEL_DESC*)Query->pOutputData;
    ULONGLONG offset, length, tableOffset, tableLength;

    if (Query->InputDataSize < sizeof(*in) || in == NULL) return STATUS_INVALID_PARAMETER;
    if (Query->OutputDataSize < sizeof(*desc) || desc == NULL) return STATUS_BUFFER_TOO_SMALL;
    if (in->LevelIndex >= BC250_WDDM_LEVEL_COUNT) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(desc, Query->OutputDataSize);
    desc->PageTableIndexBitCount = BC250_WDDM_LEVEL_BITS;
    desc->PageTableSizeInBytes = BC250_WDDM_PAGE_TABLE_BYTES;
    desc->PageTableAlignmentInBytes = 0;                // 0 means the page size of the memory segment
    if (WddmMemoryLayout(Device, &offset, &length, &tableOffset, &tableLength))
    {
        desc->PageTableSegmentId = BC250_WDDM_SEGMENT_TABLES;
        desc->PagingProcessPageTableSegmentId = BC250_WDDM_SEGMENT_TABLES;
    }
    return STATUS_SUCCESS;
}
static PAGING_PT_SHADOW_SLOT route_shadow_slots[4];

static void case_kmd_routes(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 BC250_DEVICE dev={{0x200000000ll},0x100000000ull,0x100000,NULL,0,0,0,0};
 BC250_GFX gfx={1,{0,0},&g_adev,0,NULL,0};BC250_PAGING_STREAM st;
 u32 buf[256];u64 out;unsigned written;static u64 scratch[16];
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"actual route window configured");
 memset(&st,0,sizeof(st));st.Device=&dev;st.Gfx=&gfx;st.Root=4096;st.Payload=buf;st.CommandOffset=128;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 translated=0x12345123ull;isSystem=1;
 check(PagingResolve(&st,0,64,&out) && out==(translated|PAGING_SYSTEM_ADDRESS),"actual resolver preserves system physical address and offset");
 check(PagingEmit(&st,buf,256,out,0x100000200ull,64,&written)==0 && written==83,"system to local builds mapped transfer");
 check(buf[8+3]==97 && buf[33+3]==(u32)(gfx.PagingWindow.mc+0x123) && buf[33+5]==0x200,"actual route marker includes prior command offset and uses source window");
 check(PagingEmit(&st,buf,256,0x100000200ull,out,64,&written)==0 && buf[33+5]==(u32)(gfx.PagingWindow.mc+4096+0x123),"local to system uses destination window");
 check(PagingEmit(&st,buf,256,out,out,64,&written)==0 && buf[4]!=0 && buf[6]!=0,"system to system maps two separate slots");
 st.Fill=1;st.Pattern=0xABCD1234;out=(0x12345200ull|PAGING_SYSTEM_ADDRESS);
 check(PagingEmit(&st,buf,256,0,out,64,&written)==0 && written==81 && buf[33]==SDMA_PKT_HEADER_OP(SDMA_OP_CONST_FILL) && buf[36]==st.Pattern,"system fill uses mapped transaction and preserves pattern");
 st.Fill=0;isSystem=0;translated=0x200000200ull;
 check(PagingResolve(&st,0,64,&out) && out==0x100000200ull,"local resolver retains physical-to-MC conversion");
 check(PagingEmit(&st,buf,256,out,out+64,64,&written)==0 && written==7,"local/local keeps direct packet path");
 isSystem=1;translated=0x1000000000000000ull;
 check(!PagingResolve(&st,0,64,&out),"unrepresentable system page rejected instead of truncation");
 translated=0x12345000;gfx.PagingWindowReady=0;check(!PagingResolve(&st,0,64,&out),"unavailable aperture rejected");
 {
  unsigned done=0,next,dw,totalDw=0,passes=0;int result;
  gfx.PagingWindowReady=1;fragmented=1;isSystem=1;st.CommandOffset=0;st.Fill=0;
  do {
   st.Payload=buf;
   result=PagingStreamBuild(&st,PagingResolve,PagingEmit,0,0x10000,0x30000,5*4096,done,buf,180,&dw,&next);
   check(result==PagingStreamMore || result==PagingStreamDone,"actual mapped page stream publishes valid partial batch");
   check(next>done && next<=5*4096 && dw==83*((next-done)/4096),"mapped multipass preserves data and command coordinates");
   if(dw==166) check(buf[8+3]==1 && buf[83+8+3]==250,"consecutive transactions use distinct command-position marker ranges");
   totalDw+=dw;done=next;passes++;
  }while(result==PagingStreamMore && passes<10);
  check(done==5*4096 && totalDw==5*83 && passes==3,"five scattered page pairs covered exactly in three bounded batches");
  fragmented=0;
 }
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_kmd_flush(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 u32 buf[32];ULONG written;BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 unsigned offset;NTSTATUS status;
 dev.Gfx=&gfx;gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 test_expected_vmid=1;memset(buf,0xCC,sizeof(buf));
 status=GfxPagingBuildFlush(&dev,1,buf,0,sizeof(buf),&written,&unsupported);
 check(status==STATUS_SUCCESS && written==15 && unsupported==BC250PagingSupported,"actual KMD flush builds VMID1 packet");
 check(buf[12]==2 && buf[13]==2 && all_pattern(buf+15,17,0xCCCCCCCCu),"actual KMD flush targets app VMID not GART");
 for(offset=0;offset<=4096;offset+=4) {
  unsigned cap=PagingStreamCapacity(64,offset,65536,1024,ring.funcs->align_mask,bc250_sdma_fence_size(&ring,AMDGPU_FENCE_FLAG_INT));
  memset(buf,0xCC,sizeof(buf));
  status=GfxPagingBuildFlush(&dev,1,buf,offset,64,&written,&unsupported);
  check(cap>=16 ? status==STATUS_SUCCESS && written==15 : status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && written==0,"flush accounts for earlier DMA commands and outer fence");
  if(cap<16)check(all_pattern(buf,32,0xCCCCCCCCu),"insufficient flush publishes no partial packet");
 }
 gfx.PagingReady=0;written=123;
 check(GfxPagingBuildFlush(&dev,1,buf,0,128,&written,&unsupported)==STATUS_DEVICE_NOT_READY && !written && unsupported==BC250PagingNotReady,"not-ready remains explicit helper failure");
 dev.Gfx=NULL;
 check(GfxPagingBuildFlush(&dev,1,buf,0,128,&written,&unsupported)==STATUS_DEVICE_NOT_READY && !written,"missing engine refuses flush");
 check(GfxPagingBuildFlush(&dev,0,buf,0,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"application flush refuses GART VMID");
 check(GfxPagingBuildFlush(&dev,16,buf,0,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"invalid VMID rejected");
 check(GfxPagingBuildFlush(&dev,1,buf,1,128,&written,&unsupported)==STATUS_INVALID_PARAMETER,"unaligned DMA offset rejected");
 check(!flush_lock_depth && !flush_region_depth,"all flush exits balance lifetime lock/region");
 test_expected_vmid=0;memset(hub,0,sizeof(*hub));
}

static void case_kmd_updates(void)
{
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE update={0};DXGK_PTE ptes[512];
 u32 buf[1056];ULONGLONG encoded[512],physical;static u64 scratch[16];
 ULONG written,next;unsigned i;BC250_WDDM_PAGING_UNSUPPORTED unsupported;NTSTATUS rc;
 dev.VramPhysical.QuadPart=0x200000000ll;dev.VramMcBase=0x100000000ull;dev.VramLength=0x1000000;dev.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.SegmentMapping=(unsigned char*)cpu_storage;g_VidMm.Ready=1;g_VidMm.Write=1;
 (void)PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4);g_VidMm.SegmentPhysical=0x200000000ull;g_VidMm.SegmentLength=0x1000000;
 g_VidMm.Pte.units=BC250_PTE_ADDR_PAGES;g_VidMm.Pte.aperture=BC250_PTE_VM;
 g_VidMm.Pte.system_segment=0;g_VidMm.Pte.vram_segment=1;
 g_VidMm.Pte.vram_base=g_VidMm.SegmentPhysical;g_VidMm.Pte.vram_size=g_VidMm.SegmentLength;
 update.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;update.NumPageTableEntries=512;
 update.PageTableAddress.GpuPhysical.SegmentId=1;update.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 update.pPageTableEntries=ptes;
 for(i=0;i<512;i++){ptes[i].Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);ptes[i].PageAddress=100+i;}
 check(VidMmEncodePageTable(&update,0,512,&physical,encoded) && physical==0x200001000ull,"actual encoder translates full PTE source and table physical address");
 {
  DXGK_PTE saved=ptes[0];
  ptes[0].Flags=BC250_DXGK_PTE_VALID|BC250_DXGK_PTE_CACHECOHERENT;ptes[0].PageAddress=0x123;
  check(VidMmEncodePageTable(&update,0,1,&physical,encoded) && g_VidMm.EncodedCoherentSystem==1 &&
        !g_VidMm.EncodedUncachedSystem && !g_VidMm.EncodedCoherencyMismatch && (encoded[0]&AMDGPU_PTE_SNOOPED),
        "actual encoder records coherent system leaf with matching snoop");
  ptes[0].Flags=BC250_DXGK_PTE_VALID;
  check(VidMmEncodePageTable(&update,0,1,&physical,encoded) && g_VidMm.EncodedUncachedSystem==1 &&
        !g_VidMm.EncodedCoherencyMismatch && !(encoded[0]&AMDGPU_PTE_SNOOPED),
        "actual encoder records noncoherent system leaf without snoop");
  ptes[0]=saved;
  check(VidMmEncodePageTable(&update,0,512,&physical,encoded),"restore local update oracle after coherency controls");
 }
 memset(buf,0xCC,sizeof(buf));g_VidMm.GpuCalls=0;
 rc=GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported);
 check(rc==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && next==480 && written==974,"actual builder publishes480entries then requests next buffer");
 check(buf[1]==0x1000 && buf[2]==1 && buf[3]==959,"PTE table destination converted to MC");
 for(i=0;i<480;i++)if(buf[4+2*i]!=(u32)encoded[i] || buf[5+2*i]!=(u32)(encoded[i]>>32))break;
 check(i==480 && !g_VidMm.GpuCalls,"queued first batch preserves values without CPU table write");
 check(buf[967]==1 && all_pattern(buf+974,82,0xCCCCCCCCu),"first update barrier marker and tail bounded");
 rc=GfxPagingBuildUpdate(&dev,&update,buf,0,65536,next,&written,&next,&unsupported);
 check(rc==STATUS_SUCCESS && next==512 && written==78 && buf[1]==0x1F00 && buf[3]==63,"second batch resumes table offset by480entries");
 for(i=0;i<32;i++)if(buf[4+2*i]!=(u32)encoded[480+i] || buf[5+2*i]!=(u32)(encoded[480+i]>>32))break;
 check(i==32,"second batch covers exactly remaining entries");
 memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildUpdate(&dev,&update,buf,4048,65536,0,&written,&next,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && !next && all_pattern(buf,1056,0xCCCCCCCCu),"insufficient accumulated ring budget writes nothing");
 update.NumPageTableEntries=2;update.StartIndex=17;update.Flags.Repeat=1;
 check(GfxPagingBuildUpdate(&dev,&update,buf,128,65536,0,&written,&next,&unsupported)==STATUS_SUCCESS && next==2 && buf[1]==0x1088 && buf[4]==buf[6] && buf[5]==buf[7] && buf[11]==97,"Repeat and StartIndex preserved, marker includes command offset");
 update.Flags.Repeat=0;update.StartIndex=0;update.NumPageTableEntries=512;
 ptes[511].Flags=BC250_DXGK_PTE_VALID | (2ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_INVALID_PARAMETER && !written && all_pattern(buf,1056,0xCCCCCCCCu),"invalid final source entry prevents first batch publication");
 ptes[511].Flags=ptes[0].Flags;
 gfx.PagingCpuBootstrap=1;gfx.PagingReady=0;
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_SUCCESS && !written && next==512 && g_VidMm.GpuCalls==1,"pre-RUN bootstrap uses immediate CPU update");
 gfx.PagingCpuBootstrap=0;
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_DEVICE_NOT_READY && !written && !next && g_VidMm.GpuCalls==1,"after bootstrap close not-ready cannot fall back to CPU");
 update.StartIndex=1;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects table extent overflow");
 update.StartIndex=0;update.PageTableLevel=4;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects invalid page-table level");
 check(!flush_lock_depth && !flush_region_depth,"PTE update exits balance lifetime locks");
}

static void case_cpu_updates(void)
{
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE ptes[512];
 BC250_DEVICE dev={0};BC250_GFX gfx={0};u32 output[16];
 ULONGLONG encoded[512],physical;ULONG written,next;BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 unsigned i;long long priorWritten;int maps;ULONGLONG shadowEntry;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=1;
 u.PageTableAddress.GpuPhysical.SegmentOffset=4096;u.NumPageTableEntries=2;u.StartIndex=5;u.pPageTableEntries=ptes;
 for(i=0;i<512;i++){ptes[i].Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);ptes[i].PageAddress=100+i;}
 memset(cpu_table,0xCC,4096);priorWritten=g_VidMm.Written;
 check(VidMmEncodePageTable(&u,0,2,&physical,encoded),"CPU control encoded");
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && cpu_table[5]==encoded[0] && cpu_table[6]==encoded[1] && g_VidMm.Written==priorWritten+2,"actual CPU physical update writes requested entries");
 check(cpu_table[4]==0xCCCCCCCCCCCCCCCCull && cpu_table[7]==0xCCCCCCCCCCCCCCCCull && !cpu_map_live,"CPU update bounds and lifetime preserved");
 memset(cpu_table,0xCC,4096);g_VidMm.SegmentMapping=NULL;priorWritten=g_VidMm.Written;
 check(VidMmUpdatePageTable(&u)==STATUS_DEVICE_NOT_READY && g_VidMm.Written==priorWritten && !cpu_map_live,"missing retained mapping refuses without write");
 dev.Gfx=&gfx;gfx.PagingCpuBootstrap=1;
 check(GfxPagingBuildUpdate(&dev,&u,output,0,sizeof(output),0,&written,&next,&unsupported)==STATUS_DEVICE_NOT_READY && !written && !next,"bootstrap preserves zero progress after missing retained mapping");
 g_VidMm.SegmentMapping=(unsigned char*)cpu_storage;maps=cpu_map_calls;u.NumPageTableEntries=512;u.StartIndex=0;
 ptes[511].Flags=BC250_DXGK_PTE_VALID | (2ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_map_calls==maps,"late invalid PTE rejected before destination mapping");
 for(i=0;i<512;i++)if(cpu_table[i]!=0xCCCCCCCCCCCCCCCCull)break;
 check(i==512,"late invalid PTE leaves complete destination unchanged");
 ptes[511].Flags=ptes[0].Flags;u.NumPageTableEntries=3;u.StartIndex=9;u.Flags.Repeat=1;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_table;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && cpu_table[9]==encoded[0] && cpu_table[10]==encoded[0] && cpu_table[11]==encoded[0] && cpu_map_calls==maps && !cpu_map_live,"CPU_VIRTUAL Repeat uses provided pointer without map/unmap");
 check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,9,&shadowEntry)==PAGING_PT_OK && shadowEntry==encoded[0],"CPU_VIRTUAL initialization registers physical identity and encoded entry");
 check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,8,&shadowEntry)==PAGING_PT_MISSING,"partial initialization does not invent adjacent PTEs");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical.SegmentId=1;u.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 ptes[0].PageAddress=77;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,9,&shadowEntry)==PAGING_PT_OK && shadowEntry==cpu_table[9],"immediate bootstrap updates known logical table with CPU table");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_table;
 // Fill registration capacity, then ensure a new physical table cannot be partly written.
 { unsigned slot;ULONGLONG value=0;
   for(slot=2;slot<=4;slot++)check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+slot*4096,0,1,&value,1)==PAGING_PT_OK,"fill small host registration capacity");
   u.PageTableAddress.CpuVirtual=cpu_storage+5*512;cpu_storage[5*512+9]=0xCCCCCCCCCCCCCCCCull;
   check(VidMmUpdatePageTable(&u)==STATUS_INSUFFICIENT_RESOURCES && cpu_storage[5*512+9]==0xCCCCCCCCCCCCCCCCull,"registration full refuses before CPU destination write");
 }
 g_VidMm.Write=0;
 check(VidMmUpdatePageTable(&u)==STATUS_DEVICE_NOT_READY,"closed CPU write gate does not claim success");
 g_VidMm.Write=1;u.Flags.Use64KBPages=1;
 check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER,"unsupported CPU large-page mode rejected");
 check(VidMmUpdatePageTable(NULL)==STATUS_INVALID_PARAMETER,"NULL CPU update refused");
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"failure paths release maps and snapshot/builder locks");
}

static void case_retained_mapping(void)
{
 BC250_DEVICE d={0};ULONGLONG pa,base=0x200000000ull;BOOLEAN system;int calls;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;d.VramLength=sizeof(cpu_storage);
 cpu_map_fail=0;cpu_write_setting=1;calls=cpu_map_calls;
 shadow_pool_fail=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_INSUFFICIENT_RESOURCES && !g_VidMm.Ready && !shadow_pool_live && cpu_map_calls==calls,"shadow storage failure before mapping/readiness");
 shadow_pool_fail=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS && g_VidMm.Ready && g_VidMm.Write && cpu_map_live==1 && cpu_map_calls==calls+1,"actual VidMm start acquires one retained segment mapping");
 memset(cpu_storage,0,sizeof(cpu_storage));
 cpu_storage[512]=(base+8192)|7;cpu_storage[1024]=(base+12288)|7;cpu_storage[1536]=(base+16384)|7;
 cpu_storage[2048]=(base+24576)|bc250_pte_vm_flags(1,1,0,0);
 calls=cpu_map_calls;
 check(VidMmTranslateRetained(base+4096,0x123,&pa,&system) && pa==base+24576+0x123 && !system,"actual retained walk resolves local leaf and offset");
 cpu_storage[2048]=0x100000|bc250_pte_vm_flags(1,1,1,1);
 check(VidMmTranslateRetained(base+4096,0x234,&pa,&system) && pa==0x100234 && system,"retained walk resolves system leaf without mapping host data");
 cpu_storage[1024]=0;
 check(!VidMmTranslateRetained(base+4096,0,&pa,&system) && !pa,"retained walk refuses missing directory");
 check(!VidMmTranslateRetained(base+sizeof(cpu_storage),0,&pa,&system),"retained walk bounds root against segment");
 check(cpu_map_calls==calls && cpu_map_live==1,"walk performs no dynamic map/unmap");
 VidMmStop();
 check(!g_VidMm.SegmentMapping && !g_VidMm.Ready && !g_VidMm.Write && !cpu_map_live && !shadow_pool_live && !g_VidMm.Shadow.Slots,"stop drops retained mapping and shadow after drain");
 check(!VidMmTranslateRetained(base+4096,0,&pa,&system),"post-stop translation refused");
 cpu_map_fail=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_INSUFFICIENT_RESOURCES && !g_VidMm.Ready && !g_VidMm.Write && !g_VidMm.SegmentMapping && !shadow_pool_live,"mapping resource failure occurs at start before readiness");
 cpu_map_fail=0;calls=cpu_map_calls;
 check(VidMmStart(&d,4096,sizeof(cpu_storage),1)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"invalid segment extent rejected before mapping");
 cpu_write_setting=0;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS && !g_VidMm.SegmentMapping && !g_VidMm.Write,"closed write gate retains diagnostic start without mapping");
 VidMmStop();cpu_write_setting=1;
 check(!cpu_map_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"mapping lifecycle balances ownership");
}

/* Construction ordering acceptance, also run by the default routing suite. */
static void case_queued_pte_ordering(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_STREAM stream={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte,ptes[512];
 DXGKARG_BUILDPAGINGBUFFER build={0};
 u32 commands[1056],priv[1100],dma[1056];ULONGLONG base=0x200000000ull,pa,entry;BOOLEAN system;
 PAGING_U64 mc=0;ULONG written,next;unsigned i;
 BC250_WDDM_PAGING_UNSUPPORTED unsupported;static u64 scratch[16];
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;
 d.VramLength=sizeof(cpu_storage);d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"ordering probe start");
 memset(cpu_storage,0,sizeof(cpu_storage));
 // Actual immediate CPU initialization registers all four pinned table identities.
 pte.Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i==4?6:i+1;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"register initialized paging hierarchy");
 }
 stream.Device=&d;stream.Gfx=&gfx;stream.Root=base+4096;use_retained_walk=1;
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"positive control logical resolver sees initialized page6");
 pte.PageAddress=7;u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
 u.PageTableAddress.GpuPhysical.SegmentId=1;u.PageTableAddress.GpuPhysical.SegmentOffset=16384;
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_SUCCESS && next==1 && written==16,"actual GPU update builder stages remap to page7");
 check(PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"unpublished candidate cannot change construction state");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 memset(dma,0xCC,sizeof(dma));
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);
 build.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES+written*4-1;build.UpdatePageTable=u;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER,"short private space refuses publication");
 check(build.pDmaBuffer==dma && build.pDmaBufferPrivateData==priv && all_pattern(dma,1056,0xCCCCCCCCu) && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"private refusal leaves DMA pointers, bytes and logical state untouched");
 build.DmaBufferPrivateDataSize=sizeof(priv);build.DmaSize=written*4-1;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER,"short DMA space refuses publication");
 build.DmaSize=sizeof(dma);build.DmaBufferGpuVirtualAddress=~0ull-16;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_INVALID_PARAMETER && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"private address overflow cannot advance logical state");
 build.DmaBufferGpuVirtualAddress=0;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,2)==STATUS_INVALID_PARAMETER && !priv[0] && PagingResolve(&stream,0,4096,&mc) && mc==d.VramMcBase+24576,"invalid logical batch invalidates candidate without advance");
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_SUCCESS,"actual publication helper commits valid batch");
 check(build.pDmaBuffer==dma+written && build.DmaSize==sizeof(dma)-written*4 && build.pDmaBufferPrivateData==(unsigned char*)priv+PAGING_PRIVATE_HEADER_BYTES+written*4 && memcmp(dma,commands,written*4)==0,"publication copies exact command bytes and advances both capacities");
 check(PagingResolve(&stream,0,4096,&mc),"logical resolver accepts published update before GPU executes");
 printf("QUEUED_UPDATE: wanted MC=0x%llX, builder resolved MC=0x%llX\n",d.VramMcBase+28672,(unsigned long long)mc);
 check(mc==d.VramMcBase+28672,"REQUIRED: following transfer construction resolves queued page7");
 check(VidMmTranslateRetained(base+4096,0,&pa,&system) && pa==base+24576 && !system,"GPU-visible table remains page6 until GPU executes queued update");
 cpu_storage[2048]=(u64)commands[4]|((u64)commands[5]<<32);
 check(VidMmTranslateRetained(base+4096,0,&pa,&system) && pa==base+28672,"modeled GPU payload application catches up with construction view");
 // Full update must expose only each accepted multipass prefix.
 for(i=0;i<512;i++){ptes[i].Flags=pte.Flags;ptes[i].PageAddress=6+(i&1);}
 u.NumPageTableEntries=512;u.pPageTableEntries=ptes;
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),0,&written,&next,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && next==480,"stage first480-entry logical batch");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);build.UpdatePageTable=u;
 check(WddmPublishPagingRecord(&build,written,TRUE,0,next)==STATUS_SUCCESS,"publish first multipass prefix");
 check(PagingPtShadowRead(&g_VidMm.Shadow,base+16384,479,&entry)==PAGING_PT_OK && PagingPtShadowRead(&g_VidMm.Shadow,base+16384,480,&entry)==PAGING_PT_MISSING,"first prefix cannot expose not-yet-built suffix");
 check(GfxPagingBuildUpdate(&d,&u,commands,0,sizeof(commands),480,&written,&next,&unsupported)==STATUS_SUCCESS && next==512,"stage remaining32-entry batch");
 memcpy(priv+PAGING_PRIVATE_HEADER_BYTES/4,commands,written*4);
 build.pDmaBuffer=dma;build.pDmaBufferPrivateData=priv;build.DmaSize=sizeof(dma);build.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmPublishPagingRecord(&build,written,TRUE,480,next)==STATUS_SUCCESS,"publish second multipass prefix");
 for(i=0;i<512;i++)check(PagingResolve(&stream,(PAGING_U64)i*4096,4096,&mc) && mc==d.VramMcBase+(6+(i&1))*4096,"logical whole-table multipass translation");
 // An unregistered application page must not consume a pinned paging-table slot.
 u.NumPageTableEntries=1;u.pPageTableEntries=&pte;u.PageTableAddress.GpuPhysical.SegmentOffset=5*4096;
 i=g_VidMm.Shadow.Used;
 check(VidMmCommitPagingUpdate(&u,0,1)==STATUS_SUCCESS && g_VidMm.Shadow.Used==i,"ordinary application PTE updates do not register logical tables");
 check(!VidMmTranslateRetainedPaging(base+5*4096,0,&pa,&system),"unknown logical root never falls back to live VRAM");
 use_retained_walk=0;VidMmStop();
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"ordering probe cleans lifetime state");
}

static void case_copy_range_builder(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};static u64 scratch[16];
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE range={0};BC250_WDDM_PAGING_UNSUPPORTED unsupported;
 u32 buf[64];ULONG written;ULONGLONG src,dst;unsigned cap;
 d.Gfx=&gfx;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 range.NumPageTableEntries=512;range.SrcPageTableAddress=0x10000;range.DstPageTableAddress=0x20000;
 translated=0x200004000ull;isSystem=0;translationOk=1;fragmented=0;use_retained_walk=0;
 memset(buf,0xCC,sizeof(buf));
 check(GfxPagingBuildCopyRange(&d,0x200001000ull,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_SUCCESS && written==34 && src==translated && dst==translated,"actual copy-range builder uses retained staging and reports resolved physical identity");
 check(buf[3]==0x4000 && buf[4]==1 && buf[5]==0x80000 && buf[6]==1 && buf[22]==0x4000,"copy-range builder converts resolved PA to MC, not raw GPU VA");
 check(all_pattern(buf+34,30,0xCCCCCCCCu),"range builder preserves output tail");
 range.NumPageTableEntries=2;range.SrcStartPteIndex=3;range.DstStartPteIndex=7;translation_by_offset=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,128,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_SUCCESS && src==translated+24 && dst==translated+56 && buf[1]==15 && buf[3]==0x4018 && buf[22]==0x4038 && buf[10]==97 && buf[27]==98,"copy-range indices and command-offset marker sequence are independent");
 translation_by_offset=0;range.NumPageTableEntries=512;range.SrcStartPteIndex=range.DstStartPteIndex=0;
 for(cap=0;cap<192;cap+=4) {
  memset(buf,0xCC,sizeof(buf));
  check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,cap,&written,&src,&dst,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && !src && !dst && all_pattern(buf,64,0xCCCCCCCCu),"range capacity refusal publishes no commands or identities");
 }
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,3984,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written,"accumulated ring bytes plus outer fence bound copy range");
 range.SrcStartPteIndex=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER,"source index plus count cannot exceed advertised table");
 range.SrcStartPteIndex=0;range.DstPageTableAddress++;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER,"copy table GPU VA must have documented64KiB alignment");
 range.DstPageTableAddress--;translationOk=0;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER && unsupported==BC250PagingNoTranslation,"missing logical translation refuses range");
 translationOk=1;isSystem=1;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_INVALID_PARAMETER && unsupported==BC250PagingSystemMemory,"current local-only page-table segment requires local copy addresses");
 isSystem=0;gfx.PagingCopyStaging.size=0;
 check(GfxPagingBuildCopyRange(&d,1,&range,buf,0,sizeof(buf),&written,&src,&dst,&unsupported)==STATUS_DEVICE_NOT_READY && unsupported==BC250PagingNotReady,"missing staging resource refuses before output");
 check(!flush_lock_depth && !flush_region_depth,"range builder balances lifetime lock on all exits");
}

static void case_copy_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};static u64 scratch[16];
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE ranges[40];DXGKARG_BUILDPAGINGBUFFER b={0};
 u32 dma[1100],priv[1500];u64 values[41],entry;unsigned i,prior,passes=0;
 const u64 physical=0x200004000ull;
 d.Gfx=&gfx;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 translated=physical;isSystem=0;translationOk=1;fragmented=0;use_retained_walk=0;translation_by_offset=1;
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=g_VidMm.Write=1;
 (void)PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4);
 for(i=0;i<41;i++)values[i]=100+i;
 check(PagingPtShadowApply(&g_VidMm.Shadow,physical,0,41,values,1)==PAGING_PT_OK,"copy publication initialized source/destination control");
 memset(ranges,0,sizeof(ranges));
 for(i=0;i<40;i++) {
  ranges[i].NumPageTableEntries=1;ranges[i].SrcPageTableAddress=0x10000;ranges[i].DstPageTableAddress=0x20000;
  ranges[i].SrcStartPteIndex=i;ranges[i].DstStartPteIndex=i+1;
 }
 b.CopyPageTableEntries.NumRanges=40;b.CopyPageTableEntries.pRanges=ranges;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.DmaBufferGpuVirtualAddress=~0ull-16;
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER && b.MultipassOffset==0 && b.pDmaBuffer==dma && !priv[0],"copy private header refusal preserves progress and invalidates record");
 check(PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_OK && entry==101 && all_pattern(dma,1100,0xCCCCCCCCu),"refused copy leaves logical state and DMA unchanged");
 b.DmaBufferGpuVirtualAddress=0;b.DmaSize=192;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && b.MultipassOffset==1 && b.DmaSize==56 && b.DmaBufferWriteOffset==0,"short buffer accepts exactly one whole range and preserves input offset");
 check(PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_OK && entry==100 && PagingPtShadowRead(&g_VidMm.Shadow,physical,2,&entry)==PAGING_PT_OK && entry==102,"accepted prefix visible and unaccepted suffix unchanged");
 while(b.MultipassOffset<40 && passes++<5) {
  NTSTATUS status;
  prior=b.MultipassOffset;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  b.DmaBufferWriteOffset=64;
  status=WddmBuildPagingCopies(&d,1,&b);
  check(b.MultipassOffset>prior && b.DmaBufferWriteOffset==64,"copy multipass makes progress and restores caller offset");
  check(status==(b.MultipassOffset==40?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER),"copy list reports insufficient until all ranges accepted");
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,(unsigned)(sizeof(dma)-b.DmaSize),0,NULL,NULL),"accepted copy records cover exact contiguous command span");
  for(i=1;i<=40;i++)check(PagingPtShadowRead(&g_VidMm.Shadow,physical,i,&entry)==PAGING_PT_OK && entry==(i<=b.MultipassOffset?100:100+i),"dependent range sees prior accepted logical copy across buffer boundary");
 }
 check(b.MultipassOffset==40 && passes==2,"live ring limit forces list into two further buffers");
 check(VidMmCommitPagingCopy(physical+1,physical+8,1)==STATUS_INVALID_PARAMETER,"unaligned physical copy identity refused");
 check(VidMmCommitPagingCopy(physical+4088,physical+8,2)==STATUS_INVALID_PARAMETER,"logical copy cannot cross table boundary");
 check(VidMmCommitPagingCopy(physical+4096,physical+8,1)==STATUS_SUCCESS && PagingPtShadowRead(&g_VidMm.Shadow,physical,1,&entry)==PAGING_PT_MISSING,"unknown source clears registered destination knowledge");
 check(VidMmCommitPagingCopy(physical,physical+8192,1)==STATUS_SUCCESS && g_VidMm.Shadow.Used==1,"ordinary application destination never registered by copy");
 g_VidMm.Ready=0;
 check(VidMmCommitPagingCopy(physical,physical+8,1)==STATUS_DEVICE_NOT_READY,"post-stop copy commit refuses");
 b.MultipassOffset=0;b.CopyPageTableEntries.NumRanges=1;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_DEVICE_NOT_READY && b.MultipassOffset==0 && b.pDmaBuffer==dma && !priv[0] && b.DmaBufferWriteOffset==64 && all_pattern(dma,1100,0xCCCCCCCCu),"failed logical commit cannot publish DMA or advance copy progress");
 b.CopyPageTableEntries.NumRanges=0;
 check(WddmBuildPagingCopies(&d,0,&b)==STATUS_SUCCESS && b.MultipassOffset==0,"empty range list is a completed no-op");
 b.CopyPageTableEntries.NumRanges=1;b.CopyPageTableEntries.pRanges=NULL;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER,"nonempty range list requires array");
 b.CopyPageTableEntries.NumRanges=0;b.MultipassOffset=1;
 check(WddmBuildPagingCopies(&d,1,&b)==STATUS_INVALID_PARAMETER,"out-of-list progress refused");
 check(!cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"copy publication exits balance locks");
 translation_by_offset=0;
}

static void case_copy_real_walk(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 DXGK_BUILDPAGINGBUFFER_COPY_RANGE ranges[2]={{0}};DXGKARG_BUILDPAGINGBUFFER b={0};
 static u64 scratch[16];u64 staging[512],pa,expected;BOOLEAN system;
 u32 dma[256],priv[300];unsigned i;const u64 base=0x200000000ull;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;
 d.VramLength=sizeof(cpu_storage);d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 gfx.PagingCopyStaging.size=4096;gfx.PagingCopyStaging.mc=0x100080000ull;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"real-copy walker startup");
 memset(cpu_storage,0,sizeof(cpu_storage));
 pte.Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=7;i++) {
  u.PageTableLevel=i<4?4-i:0;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  pte.PageAddress=i<4?i+1:i==5?7:i==6?5:4;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"real-copy CPU initialization registers table identities");
 }
 u.PageTableAddress.CpuVirtual=cpu_storage+4*512;
 for(i=1;i<=3;i++) {
  u.StartIndex=i*16;pte.PageAddress=i+3;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"real-copy maps64KiB-aligned VAs to distinct tables");
 }
 expected=cpu_storage[7*512];use_retained_walk=1;
 check(VidMmTranslatePaging(base+4096,0x30000,&pa,&system) && pa==base+6*4096 && !system,"positive control source VA initially names physical page6");
 ranges[0].NumPageTableEntries=1;ranges[0].SrcPageTableAddress=0x20000;
 ranges[0].DstPageTableAddress=0x10000;ranges[0].DstStartPteIndex=48;
 ranges[1].NumPageTableEntries=1;ranges[1].SrcPageTableAddress=0x30000;
 ranges[1].DstPageTableAddress=0x20000;ranges[1].DstStartPteIndex=1;
 b.CopyPageTableEntries.NumRanges=2;b.CopyPageTableEntries.pRanges=ranges;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPagingCopies(&d,base+4096,&b)==STATUS_SUCCESS && b.MultipassOffset==2,"actual list builder accepts two address-dependent copies");
 check(dma[3]==5*4096 && dma[22]==4*4096+48*8,"first command copies remap PTE into actual leaf table");
 check(dma[34+3]==7*4096 && dma[34+22]==5*4096+8,"second command resolves new source physical page7 through actual logical walker");
 check(VidMmTranslatePaging(base+4096,0x30000,&pa,&system) && pa==base+7*4096,"logical hierarchy reflects accepted remap before execution");
 check(PagingPtShadowRead(&g_VidMm.Shadow,base+5*4096,1,&pa)==PAGING_PT_OK && pa==expected,"second accepted copy metadata comes from newly mapped source page");
 check(VidMmTranslateRetained(base+4096,0x30000,&pa,&system) && pa==base+6*4096 && cpu_storage[5*512+1]==0,"live GPU tables unchanged by command construction");
 // Independent data execution model: decode the two COPY_LINEAR addresses per
 // transaction, using staging as a separate host allocation. Not a GPU emulator.
 for(i=0;i<2;i++) {
  const u32*packet=dma+i*34;u64 source=((u64)packet[4]<<32)|packet[3];
  u64 destination=((u64)packet[23]<<32)|packet[22];unsigned bytes=packet[1]+1;
  check(bytes==8 && source>=d.VramMcBase && source-d.VramMcBase<=sizeof(cpu_storage)-bytes && destination>=d.VramMcBase && destination-d.VramMcBase<=sizeof(cpu_storage)-bytes,"interpreted copy addresses within host VRAM model");
  if(bytes==8 && source>=d.VramMcBase && source-d.VramMcBase<=sizeof(cpu_storage)-bytes && destination>=d.VramMcBase && destination-d.VramMcBase<=sizeof(cpu_storage)-bytes) {
   memcpy(staging,(unsigned char*)cpu_storage+(size_t)(source-d.VramMcBase),bytes);
   memcpy((unsigned char*)cpu_storage+(size_t)(destination-d.VramMcBase),staging,bytes);
  }
 }
 check(VidMmTranslateRetained(base+4096,0x30000,&pa,&system) && pa==base+7*4096 && cpu_storage[5*512+1]==expected,"modeled execution makes live tables agree with accepted construction state");
 VidMmStop();use_retained_walk=0;
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"real-copy integration balances mapping and locks");
}

static void case_separate_table_extent(void)
{
 BC250_DEVICE d={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 D3DGPU_PHYSICAL_ADDRESS address={0};u64 physical;BOOLEAN system;unsigned i;int calls;
 const u64 base=0x200000000ull,table=base+0x100000;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;d.VramLength=0x200000;
 cpu_map_fail=0;cpu_write_setting=1;
 check(VidMmStartLayout(&d,0,0x100000,1,0x100000,sizeof(cpu_storage),3)==STATUS_SUCCESS,"disjoint table layout starts");
 check(last_map_physical==table && last_map_bytes==sizeof(cpu_storage) && g_VidMm.SegmentLength==sizeof(cpu_storage),"retained CPU mapping covers only table storage");
 memset(cpu_storage,0,sizeof(cpu_storage));u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  pte.Flags=BC250_DXGK_PTE_VALID | ((u64)(i<4?3:1)<<BC250_DXGK_PTE_SEGMENT_SHIFT);pte.PageAddress=i<4?i+1:7;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"separate hierarchy CPU initialization");
 }
 check(VidMmTranslateRetainedPaging(table+4096,0,&physical,&system) && physical==base+7*4096 && !system,"logical walker traverses table segment to application leaf");
 check(VidMmTranslateRetained(table+4096,0,&physical,&system) && physical==base+7*4096,"live walker traverses retained table map to application leaf");
 address.SegmentId=1;address.SegmentOffset=4096;
 check(!VidMmRootPhysical(&address,&physical),"application segment cannot be table root");
 address.SegmentId=3;check(VidMmRootPhysical(&address,&physical) && physical==table+4096,"table segment root uses independent origin");
 u.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;u.PageTableAddress.GpuPhysical=address;u.PageTableLevel=0;
 {u64 entry;check(VidMmEncodePageTable(&u,0,1,&physical,&entry) && physical==table+4096,"queued table destination uses table origin");}
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.PageTableAddress.CpuVirtual=cpu_storage+4*512;u.StartIndex=16;
 pte.Flags=BC250_DXGK_PTE_VALID | (3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);pte.PageAddress=4;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS && VidMmTranslateRetainedPaging(table+4096,0x10000,&physical,&system) && physical==table+4*4096,"paging leaf can map table storage as data");

 {u64 before=cpu_storage[4*512+16];unsigned used=g_VidMm.Shadow.Used;
  override_cpu_physical=base+4096;
  check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_storage[4*512+16]==before && g_VidMm.Shadow.Used==used,"CPU borrowed application page refused before write/registration");
  override_cpu_physical=table+sizeof(cpu_storage);
  check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_storage[4*512+16]==before && g_VidMm.Shadow.Used==used,"CPU borrowed page at table end refused before write/registration");
  override_cpu_physical=table+1;
  check(VidMmUpdatePageTable(&u)==STATUS_INVALID_PARAMETER && cpu_storage[4*512+16]==before,"unaligned physical table identity refused");
  override_cpu_physical=0;
 }
 VidMmStop();check(!cpu_map_live && !shadow_pool_live,"separate table stop releases retained resources");
 calls=cpu_map_calls;
 check(VidMmStartLayout(&d,0,0x100000,1,0x80000,4096,3)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls && !shadow_pool_live,"overlap refused before reservation");
 check(VidMmStartLayout(&d,0,0x100000,1,0x100000,4096,1)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"same ID cannot describe distinct ranges");
 check(VidMmStartLayout(&d,0,0x100000,1,0x200000,4096,3)==STATUS_INVALID_PARAMETER && cpu_map_calls==calls,"table extent outside VRAM refused");
 check(!cpu_lock_depth && !flush_lock_depth && !flush_region_depth,"separate table checks balance locks");
}

static void case_wddm_memory_layout(void)
{
 BC250_DEVICE d={0};u64 offset,length,tableOffset,tableLength;unsigned gib;
 d.VramEnabled=1;d.Post.Pitch=7680;d.Post.Height=1200;layout_fb_offset=0;layout_fb_known=1;
 for(gib=1;gib<=32;gib++) {
  u64 available;
  d.VramLength=(u64)gib<<30;
  check(WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"supported model VRAM yields table layout");
  check(offset>=7680ull*1200 && offset-7680ull*1200<65536,"whole framebuffer excluded with64KiB alignment");
  check(!(offset&65535) && !(length&65535) && !(tableOffset&65535) && !(tableLength&65535),"all advertised segment extents64KiB aligned");
  check(tableOffset==offset+length && tableOffset+tableLength==d.VramLength-BC250_VRAM_TOP_RESERVED,"application/table ranges disjoint and end before reserved tail");
  available=d.VramLength-BC250_VRAM_TOP_RESERVED-offset;
  check(tableLength==((available/32)&~65535ull) && length+tableLength==available,"table capacity follows explicit1/32 budget without hidden overlap");
 }
 d.VramEnabled=0;check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength) && !offset && !length && !tableOffset && !tableLength,"closed gate publishes no layout");
 d.VramEnabled=1;d.VramLength=BC250_VRAM_TOP_RESERVED;check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"no usable bytes refuses layout");
 d.VramLength=BC250_VRAM_TOP_RESERVED+16*1024*1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength) && tableLength==4*1024*1024 && length==12*1024*1024,"small layout uses explicit4MiB table minimum");
 d.VramLength=BC250_VRAM_TOP_RESERVED+4*1024*1024;
 check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"table minimum cannot consume application segment entirely");
 layout_fb_known=1;layout_fb_offset=~0ull-8;d.VramLength=8ull<<30;
 check(!WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"foreign framebuffer extent rejected without wrap");
 layout_fb_offset=0;
}

static void case_wddm_segment_queries(void)
{
 BC250_DEVICE d={0};DXGKARG_QUERYADAPTERINFO q={0};DXGK_QUERYSEGMENTOUT4 out={0};
 DXGK_QUERYPAGETABLELEVELDESCIN in={0};DXGK_PAGE_TABLE_LEVEL_DESC level;
 enum {stride=sizeof(DXGK_SEGMENTDESCRIPTOR4)+16};u64 storage[(stride*3+7)/8];
 DXGK_SEGMENTDESCRIPTOR4 *app,*aperture,*tables;u64 offset,length,tableOffset,tableLength;unsigned i,j;
 d.VramEnabled=1;d.VramLength=8ull<<30;d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.Post.Pitch=7680;d.Post.Height=1200;
 layout_fb_known=1;layout_fb_offset=0;q.OutputDataSize=sizeof(out);q.pOutputData=&out;out.PagingBufferSize=0xABCD;
 check(WddmQuerySegment4(&d,&q)==STATUS_DEVICE_NOT_READY && !g_ApertureOffered && !out.NbSegment,
       "segment query cannot advertise unprepared aperture geometry");
 {
  struct amdgpu_bo bo={0};
  int calls=gart_capture_calls;
  g_adev.gmc.gart_start=0x300000000ull;g_adev.gmc.gart_size=512ull<<20;
  bo.gpu_addr=0x400000000ull;g_adev.gart.bo=&bo;g_adev.gart.table_size=1ull<<20;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_SUCCESS &&
        d.WddmAperture.mc==0x304002000ull && d.WddmAperture.table==0x400020010ull &&
        gart_capture_calls==calls+1 && !gart_capture_lock && !gart_capture_unlocked,
        "actual capture copies nonzero GART geometry under owner lock without requiring enabled hardware");
  gart_capture_status=STATUS_DEVICE_NOT_READY;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_DEVICE_NOT_READY && !d.WddmAperture.bytes &&
        !d.WddmAperture.mc && !d.WddmAperture.table && !gart_capture_lock,
        "failed GART setup clears cached aperture and balances lock");
  gart_capture_status=STATUS_SUCCESS;g_adev.gart.bo=NULL;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_DEVICE_NOT_READY && !d.WddmAperture.bytes &&
        !gart_capture_lock,"missing GART table refuses capture");
  g_adev.gart.bo=&bo;g_adev.gart.table_size=8;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_DEVICE_NOT_READY && !d.WddmAperture.bytes,
        "short actual GART table cannot advertise OS aperture");
  g_adev.gart.table_size=1ull<<20;
  check(GartCaptureAperture(&d,&d.WddmAperture)==STATUS_SUCCESS,"capture restored for descriptor controls");
  g_adev.gart.bo=NULL;
 }
 check(WddmQuerySegment4(&d,&q)==STATUS_SUCCESS && out.NbSegment==3 && out.PagingBufferSize==0xABCD,"count-only query advertises three segments without touching unrelated fields");
 memset(storage,0xCC,sizeof(storage));out.SegmentDescriptorStride=stride;out.pSegmentDescriptor=storage;
 check(WddmQuerySegment4(&d,&q)==STATUS_SUCCESS && g_ApertureOffered,"second query fills three descriptors");
 app=(DXGK_SEGMENTDESCRIPTOR4*)storage;aperture=(DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)storage+stride);tables=(DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)storage+2*stride);
 check(WddmMemoryLayout(&d,&offset,&length,&tableOffset,&tableLength),"query uses valid shared layout");
 check((u64)app->CpuTranslatedAddress.QuadPart==0x200000000ull+offset && (u64)app->BaseAddress.QuadPart==d.VramMcBase+offset && app->Size==length,"application descriptor addresses match shared layout");
 check((u64)tables->CpuTranslatedAddress.QuadPart==0x200000000ull+tableOffset && tables->Size==tableLength && !tables->Flags.DirectFlip && !tables->Flags.Aperture && tables->Flags.CpuVisible,"table descriptor owns separate local non-scanout extent");
 check((u64)aperture->BaseAddress.QuadPart==0x304002000ull &&
       aperture->CommitLimit==d.WddmAperture.bytes,
       "advertised OS aperture uses captured nonzero GART base plus reserved offset");
 check(aperture->Flags.Aperture && aperture->Size==BC250_WDDM_APERTURE_BYTES && out.PagingBufferSegmentId==0,"aperture ID2 and OS paging-buffer placement preserved");
 for(i=0;i<3;i++)for(j=sizeof(DXGK_SEGMENTDESCRIPTOR4);j<stride;j++)check(((UCHAR*)storage)[i*stride+j]==0xCC,"descriptor stride padding untouched");
 q.pInputData=&in;q.InputDataSize=sizeof(in);q.pOutputData=&level;q.OutputDataSize=sizeof(level);
 for(i=0;i<4;i++){in.LevelIndex=i;check(WddmPageTableLevelDesc(&d,&q)==STATUS_SUCCESS && level.PageTableSegmentId==3 && level.PagingProcessPageTableSegmentId==3 && level.PageTableSizeInBytes==4096,"all hierarchy levels use table segment3");}
 q.pOutputData=&out;q.OutputDataSize=sizeof(out);out.NbSegment=2;
 check(WddmQuerySegment4(&d,&q)==STATUS_INVALID_PARAMETER,"short descriptor count refused");
 out.NbSegment=3;out.SegmentDescriptorStride=sizeof(DXGK_SEGMENTDESCRIPTOR4)-1;
 check(WddmQuerySegment4(&d,&q)==STATUS_INVALID_PARAMETER,"short descriptor stride refused");
 d.VramEnabled=0;out.NbSegment=0;
 check(WddmQuerySegment4(&d,&q)==STATUS_SUCCESS && !out.NbSegment && !g_ApertureOffered,"closed VRAM gate advertises no segments");
}

static void case_probe_physical_copy(void)
{
 BC250_DEVICE d={0};DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 ULONG words[BC250_IB_PROBE_DWORDS];u64 leaf,physical;BOOLEAN system;unsigned i;int maps,copies;
 const u64 base=0x200000000ull;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;d.VramPhysical.QuadPart=(long long)base;d.VramLength=sizeof(cpu_storage);cpu_write_setting=1;
 check(VidMmStart(&d,0,sizeof(cpu_storage),1)==STATUS_SUCCESS,"probe initialization");
 memset(cpu_storage,0,sizeof(cpu_storage));g_VidMm.Pte.system_limit=0x1000000;
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<=4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  pte.Flags=BC250_DXGK_PTE_VALID | ((u64)(i<4?1:0)<<BC250_DXGK_PTE_SEGMENT_SHIFT);pte.PageAddress=i<4?i+1:0x123;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"probe hierarchy CPU initialized");
 }
 maps=cpu_map_calls;probe_copy_calls=probe_copy_fail=probe_copy_short=0;
 check(VidMmProbeIb(base+4096,16,&leaf,&physical,&system,words) && physical==0x123010 && system && leaf && probe_copy_address==physical && probe_copy_bytes==sizeof(words),"probe copies exact VA offset with OS physical-memory API");
 check(words[0]==0xABC00000u && words[239]==0xABC000EFu && cpu_map_calls==maps,"probe returns payload without extra physical mappings");
 check(VidMmProbeIb(base+4096,4092,&leaf,&physical,&system,words) && probe_copy_bytes==4 && words[0]==0xABC00000u && words[1]==0,"probe at page end copies only one DWORD");
 probe_copy_short=1;
 check(!VidMmProbeIb(base+4096,0,&leaf,&physical,&system,words) && !leaf && !physical && !system && words[0]==0 && words[239]==0,"short copy clears all partially returned data");
 probe_copy_short=0;probe_copy_fail=1;
 check(!VidMmProbeIb(base+4096,0,&leaf,&physical,&system,words) && words[0]==0 && !physical,"failed OS copy cannot masquerade as valid zero snapshot");
 probe_copy_fail=0;copies=probe_copy_calls;
 check(!VidMmProbeIb(base+4096,1,&leaf,&physical,&system,words) && probe_copy_calls==copies,"unaligned diagnostic address refuses before OS copy");
 VidMmStop();check(!VidMmProbeIb(base+4096,0,&leaf,&physical,&system,words) && probe_copy_calls==copies,"post-stop probe refuses before dereference/copy");
 check(!cpu_map_live && !shadow_pool_live && !flush_lock_depth && !flush_region_depth,"probe and stop release mapping and locks");
}

static void case_mdl_address(void)
{
 struct {MDL mdl;PFN_NUMBER pages[4];} m={{4*4096,0},{0x123,0x789,0x456,0xABC}};
 u64 address;unsigned page;
 for(page=0;page<4;page++) {
  check(GfxPagingMdlAddress(&m.mdl,0,(u64)page*4096,4096,&address) && address==(m.pages[page]<<12),"MDL fragmented page progress uses each PFN");
  check(GfxPagingMdlAddress(&m.mdl,page,4,8,&address) && address==(m.pages[page]<<12)+4,"MDL first-page index distinct from byte offset");
 }
 check(!GfxPagingMdlAddress(&m.mdl,4,0,1,&address) && !address,"MDL first-page outside extent refuses");
 check(!GfxPagingMdlAddress(&m.mdl,0,4095,2,&address),"MDL one-slice resolver refuses page crossing");
 check(!GfxPagingMdlAddress(&m.mdl,1,~0ull,1,&address),"MDL offset addition overflow refuses");
 m.mdl.ByteOffset=13;m.mdl.ByteCount=4096;
 check(!GfxPagingMdlAddress(&m.mdl,0,0,1,&address),"partial MDL first page prefix is not described memory");
 check(GfxPagingMdlAddress(&m.mdl,0,13,4083,&address) && address==(m.pages[0]<<12)+13,"partial MDL first page uses exact described byte interval");
 check(GfxPagingMdlAddress(&m.mdl,1,0,13,&address) && address==(m.pages[1]<<12),"partial MDL last page bounds account for initial byte offset");
 check(!GfxPagingMdlAddress(&m.mdl,1,0,14,&address) && !address,"partial MDL final byte overrun refuses");
 check(!GfxPagingMdlAddress(&m.mdl,1,13,1,&address),"MDL end is exclusive");
 m.pages[0]=1ull<<52;
 check(!GfxPagingMdlAddress(&m.mdl,0,13,1,&address) && !address,"MDL overflowing PFN refuses before address use");
 m.mdl.ByteOffset=4096;
 check(!GfxPagingMdlAddress(&m.mdl,0,4096,1,&address),"malformed MDL byte offset refuses");
 m.mdl.ByteOffset=0;m.mdl.ByteCount=0;
 check(!GfxPagingMdlAddress(&m.mdl,0,0,1,&address),"empty MDL refuses");
 check(!GfxPagingMdlAddress(NULL,0,0,1,&address) && !address,"NULL MDL clears output");
 check(!GfxPagingMdlAddress(&m.mdl,0,0,1,NULL),"NULL output refuses");
}

static void case_physical_stream(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub*hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 struct {MDL mdl;PFN_NUMBER pages[4];} src={{4*4096,0},{0x123,0x789,0x456,0xABC}};
 struct {MDL mdl;PFN_NUMBER pages[4];} dst={{4*4096,0},{0xFED,0x987,0x654,0x321}};
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT source={&src.mdl,0,3*4096,1},destination={&dst.mdl,0,3*4096,0};
 BC250_PAGING_ENDPOINT local={NULL,0x100000200ull,3*4096,0};
 u32 buf[257];static u64 scratch[16];ULONG written;ULONGLONG next,done=0;
 unsigned i,pass=0;NTSTATUS status;
 dev.Gfx=&gfx;dev.VramMcBase=0x100000000ull;dev.VramLength=0x100000;
 gfx.PagingReady=1;gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"physical stream GART window initialized");
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 do {
  memset(buf,0xCC,sizeof(buf));
  status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,128,180*4,done,&written,&next);
  check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"physical two-MDL stream accepted");
  check(next>done && next<=3*4096 && written==83*((next-done)/4096),"physical stream exact page progress");
  for(i=0;i<written;i+=83) {
   unsigned page=(unsigned)(done/4096)+i/83;
   u64 spte=((u64)buf[i+5]<<32)|buf[i+4],dpte=((u64)buf[i+7]<<32)|buf[i+6];
   check((spte&AMDGPU_PTE_ADDR_MASK)==(src.pages[page+1]<<12),"source MDL uses its own PFNs and FirstPage");
   check((dpte&AMDGPU_PTE_ADDR_MASK)==(dst.pages[page]<<12),"destination same offset selects different MDL PFNs");
   check(buf[i+36]==(u32)gfx.PagingWindow.mc && buf[i+38]==(u32)(gfx.PagingWindow.mc+4096),"physical copy uses distinct source and destination GART slots");
   check(buf[i+11]==1+3*(32+i),"physical markers include prior DMA command offset");
  }
  check(buf[180]==0xCCCCCCCCu,"physical stream respects command capacity canary");
  done=next;pass++;
 }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<4);
 check(done==3*4096 && pass==2,"physical fragmented transfer completed in two buffers");
 status=GfxPagingBuildPhysical(&dev,&source,&local,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==83 && next==64 && buf[38]==(u32)local.Address,"MDL to local keeps already-MC destination");
 status=GfxPagingBuildPhysical(&dev,&local,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==83 && buf[36]==(u32)local.Address,"local to MDL keeps already-MC source");
 status=GfxPagingBuildPhysical(&dev,&local,&local,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==7 && next==64,"physical local pair uses direct packet path");
 status=GfxPagingBuildPhysical(&dev,NULL,&destination,TRUE,64,0x87654321,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==81 && buf[36]==0x87654321,"physical MDL fill needs no source and preserves pattern");
 status=GfxPagingBuildPhysical(&dev,NULL,&local,TRUE,64,0x87654321,buf,0,1024,0,&written,&next);
 check(status==STATUS_SUCCESS && written==5 && next==64,"physical local fill uses direct packet path");
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,82*4,4096,&written,&next);
 check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && next==4096,"physical insufficient capacity preserves input progress");
 // A small buffer must not publish a valid prefix of an invalid total extent.
 memset(buf,0xCC,sizeof(buf));
 source.Length=4*4096;destination.Length=4*4096;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,4*4096,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next && all_pattern(buf,257,0xCCCCCCCCu),"whole MDL range accounts for FirstPage before first packet");
 source.Length=3*4096;destination.Length=3*4096;
 dst.mdl.ByteCount=3*4096-1;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next && all_pattern(buf,257,0xCCCCCCCCu),"destination short final MDL byte refuses before first packet");
 dst.mdl.ByteCount=3*4096;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && written==83 && next==4096,"exact MDL end permits bounded first batch");
 dst.mdl.ByteCount=4*4096;
 local.Address=dev.VramMcBase+dev.VramLength-4096;
 memset(buf,0xCC,sizeof(buf));
 status=GfxPagingBuildPhysical(&dev,&source,&local,FALSE,8192,0,buf,0,96*4,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next && all_pattern(buf,257,0xCCCCCCCCu),"whole local range crossing VRAM end refuses before first packet");
 check(GfxPagingEndpointValid(&dev,&local,4096),"exact local VRAM end is valid");
 check(!GfxPagingEndpointValid(&dev,&local,4097),"local VRAM end plus one byte refuses");
 local.Address=dev.VramMcBase-1;
 check(!GfxPagingEndpointValid(&dev,&local,1),"local address below VRAM base refuses");
 local.Address=~0ull-4095;dev.VramMcBase=local.Address;dev.VramLength=8192;
 check(!GfxPagingEndpointValid(&dev,&local,8192),"local interval arithmetic overflow refuses");
 local.Address=PAGING_SYSTEM_ADDRESS-4096;dev.VramMcBase=local.Address;
 check(!GfxPagingEndpointValid(&dev,&local,8192),"local interval cannot cross system marker bit");
 dev.VramMcBase=0x100000000ull;dev.VramLength=0x100000;local.Address=0x100000200ull;
 src.mdl.ByteOffset=13;source.FirstPage=0;
 check(!GfxPagingEndpointValid(&dev,&source,64),"physical endpoint zero progress cannot cover undescribed MDL prefix");
 source.FirstPage=1;
 check(GfxPagingEndpointValid(&dev,&source,3*4096),"MDL first page skips partial prefix while preserving valid tail");
 src.mdl.ByteOffset=0;
 src.pages[2]=1ull<<40;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,3*4096,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"unrepresentable second source PFN rolls back tentative first packet");
 src.pages[2]=0x456;
 destination.Length=63;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"endpoint extent preflight refuses overrun");
 destination.Length=3*4096;local.Address=dev.VramMcBase+dev.VramLength-32;
 status=GfxPagingBuildPhysical(&dev,&source,&local,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"local endpoint cannot cross device VRAM end");
 gfx.PagingWindowReady=0;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"physical system transfer requires reserved GART window");
 gfx.PagingReady=0;
 status=GfxPagingBuildPhysical(&dev,&source,&destination,FALSE,64,0,buf,0,1024,0,&written,&next);
 check(status==STATUS_INVALID_PARAMETER && !written && !next,"physical builder refuses unready engine");
 check(!flush_region_depth && !flush_lock_depth,"physical builder balances lifetime lock on all paths");
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_physical_fill_ddi(void)
{
 BC250_DEVICE dev={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT endpoint;DXGKARG_BUILDPAGINGBUFFER b={0};
 ULONGLONG app,appLength,table,tableLength,moved,total,done,address;
 ULONG token;unsigned mode,passes;u32 dma[32],priv[64];NTSTATUS status;
 dev.Gfx=&gfx;dev.VramMcBase=0x100000000ull;dev.VramLength=16ull<<30;dev.VramEnabled=1;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 layout_fb_known=0;
 check(WddmMemoryLayout(&dev,&app,&appLength,&table,&tableLength),"physical fill obtains advertised layout");
 check(WddmLocalPagingEndpoint(&dev,1,dev.VramMcBase+app,28,64,&endpoint) && endpoint.Address==dev.VramMcBase+app+28,"local endpoint adds byte offset without doubling MC base");
 check(!WddmLocalPagingEndpoint(&dev,1,dev.VramMcBase+table,0,64,&endpoint),"application endpoint cannot name table segment");
 check(!WddmLocalPagingEndpoint(&dev,3,dev.VramMcBase+app,0,64,&endpoint),"table endpoint cannot name application segment");
 check(WddmLocalPagingEndpoint(&dev,3,dev.VramMcBase+table,0,tableLength,&endpoint),"table endpoint accepts exact advertised extent");
 check(!WddmLocalPagingEndpoint(&dev,3,dev.VramMcBase+table,0,tableLength+1,&endpoint),"table endpoint refuses reserved tail");
 check(!WddmLocalPagingEndpoint(&dev,2,0,0,64,&endpoint),"aperture endpoint is not reinterpreted as VRAM");
 check(!WddmLocalPagingEndpoint(&dev,1,dev.VramMcBase+app,~0ull,64,&endpoint),"segment byte offset overflow refuses");
 for(mode=0;mode<2;mode++) {
  total=mode ? (1ull<<32)+8192+36 : 3*4096+36;
  token=mode ? 1u<<20 : 0;done=mode ? (1ull<<32)-28 : 0;passes=0;
  address=dev.VramMcBase+app+28;
  do {
   ULONGLONG expected=4096-((address+done)&4095);
   if(expected>total-done)expected=total-done;
   memset(&b,0,sizeof(b));memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
   b.DmaBufferWriteOffset=128;b.DmaBufferGpuVirtualAddress=0x700000000ull;b.MultipassOffset=token;
   b.Fill.FillSize=(SIZE_T)total;b.Fill.FillPattern=0xA5C31234;b.Fill.Destination.SegmentId=1;
   b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)address;
   status=WddmBuildPhysicalFill(&dev,&b,&moved);
   check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"physical Fill DDI adapter accepts batch");
   check(moved==expected && moved>0 && b.MultipassOffset==token+1,"fill token counts page slices including partial first/last");
   check((((u64)dma[2]<<32)|dma[1])==address+done && dma[3]==0xA5C31234 && dma[4]+1==expected,"published fill packet has independent expected address pattern and byte count");
   check(b.DmaSize==16*4-20 && b.pDmaBuffer==dma+5 && b.DmaBufferWriteOffset==128,"fill publication advances remaining DMA without changing input command offset");
   check(all_pattern(dma+5,27,0xCCCCCCCCu),"fill command tail canary intact");
   check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,20,0,NULL,NULL),"fill private record covers exactly the published command");
   done+=moved;token=b.MultipassOffset;passes++;
   check((status==STATUS_SUCCESS)==(done==total),"fill completion only after exact final byte");
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && passes<8);
  check(done==total && passes==(mode?3u:4u),"unaligned and >4GiB resumed fill finish in expected page slices");
 }
 b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.MultipassOffset=0;b.Fill.FillSize=64;b.Fill.Destination.SegmentId=3;
 b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)(dev.VramMcBase+table+64);
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=1;g_VidMm.Write=1;
 g_VidMm.SegmentPhysical=(u64)dev.VramPhysical.QuadPart+table;g_VidMm.SegmentLength=tableLength;
 check(PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4)==PAGING_PT_OK,"table fill shadow initialized");
 {
  u64 old=0x123456789ABCDEF0ull,read;
  check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,1,&old,1)==PAGING_PT_OK,"table fill known entry registered");
  b.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES;
  check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved &&
        PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,&read)==PAGING_PT_OK && read==old,
        "private refusal leaves logical table fill unpublished");
  b.DmaBufferPrivateDataSize=sizeof(priv);
 }

 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_SUCCESS && moved==64 && b.MultipassOffset==1 &&
       (((u64)dma[2]<<32)|dma[1])==dev.VramMcBase+table+64,"physical table-segment fill preserves full MC destination");
 {
  u64 read;
  check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,&read)==PAGING_PT_OK && read==0xA5C31234A5C31234ull,"accepted table fill updates logical known entry before later builds");
  check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,15,&read)==PAGING_PT_OK && read==0xA5C31234A5C31234ull,"full unknown PTE covered by fill becomes known");
 }
 b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.MultipassOffset=0;b.DmaBufferGpuVirtualAddress=~0ull-8;b.Fill.FillPattern=0x11112222;
 memset(dma,0xCC,sizeof(dma));
 {
  u64 read;
  check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
        PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,8,&read)==PAGING_PT_OK && read==0xA5C31234A5C31234ull &&
        all_pattern(dma,32,0xCCCCCCCCu),"private header address refusal leaves table shadow and DMA untouched");
 }
 b.DmaBufferGpuVirtualAddress=0x700000000ull;g_VidMm.Ready=0;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_DEVICE_NOT_READY && !moved && !b.MultipassOffset && !priv[0] &&
       b.pDmaBuffer==dma && all_pattern(dma,32,0xCCCCCCCCu),"shadow commit refusal invalidates private record without DMA or progress publication");
 memset(&g_VidMm,0,sizeof(g_VidMm));
 b.Fill.Destination.SegmentId=1;
 // Refusals must not advance DMA or the external token.
 b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.MultipassOffset=0;b.Fill.FillSize=8192;b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)(dev.VramMcBase+table-4096);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset && b.pDmaBuffer==dma && all_pattern(dma,32,0xCCCCCCCCu),"whole application fill cannot cross into table segment");
 b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)(dev.VramMcBase+app+28);b.Fill.FillSize=64;
 b.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset && b.pDmaBuffer==dma,"private capacity refusal retains fill token and DMA pointer");
 b.DmaBufferPrivateDataSize=sizeof(priv);b.MultipassOffset=2;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved,"fill token beyond final slice refuses");
 b.MultipassOffset=0;b.Fill.FillSize=65;
 check(WddmBuildPhysicalFill(&dev,&b,&moved)==STATUS_INVALID_PARAMETER && !moved,"non-DWORD fill refuses rather than truncates");
 layout_fb_known=1;
}

static void case_virtual_fill_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[256],priv[300];
 u64 app,len,table,tableLen,moved,read,zero=0,physical;BOOLEAN system;
 ULONG written;NTSTATUS status;unsigned pass;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramLength=16ull<<30;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&len,&table,&tableLen),"virtual fill layout positive control");
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=1;g_VidMm.Write=1;
 g_VidMm.SegmentPhysical=(u64)d.VramPhysical.QuadPart+table;g_VidMm.SegmentLength=tableLen;
 check(PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4)==PAGING_PT_OK,"virtual fill logical storage ready");
 check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,1,&zero,1)==PAGING_PT_OK,"virtual fill table identity registered");
 translated=g_VidMm.SegmentPhysical;translation_by_offset=1;isSystem=0;translationOk=1;
 b.FillVirtual.FillSizeInBytes=8184;b.FillVirtual.DestinationVirtualAddress=0x40004;
 b.FillVirtual.FillPattern=0x12345678;b.DmaBufferWriteOffset=128;
 for(pass=0;pass<2;pass++) {
  memset(dma,0xCC,sizeof(dma));b.pDmaBuffer=dma;b.DmaSize=64;
  b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildVirtualFill(&d,4096,&b,&moved);
  check(status==(pass?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && moved==4092,"virtual fill publishes one partial page per small buffer");
  check(b.MultipassOffset==(pass?2u:1u) && b.DmaBufferWriteOffset==128,"virtual fill preserves slice resume and restores command offset");
  check((((u64)dma[2]<<32)|dma[1])==d.VramMcBase+table+(pass?0:4) && dma[3]==0x12345678 && dma[4]==4091,"virtual fill packet uses captured physical table identity and byte count");
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,20,0,NULL,NULL),"virtual fill accepted private record covers command");
  check(all_pattern(dma+5,251,0xCCCCCCCCu),"virtual fill leaves command tail untouched");
  check(PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,&read)==PAGING_PT_OK &&
    read==(pass?0x1234567812345678ull:0x1234567800000000ull),"virtual fill updates correct half before subsequent construction");
 }
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=128;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_SUCCESS && moved==8184 && b.MultipassOffset==2 &&
       b.DmaBufferWriteOffset==128 && b.DmaSize==88,"two virtual page slices share one accumulated DMA buffer");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,40,0,NULL,NULL),"two virtual fill records cover contiguous command offsets");
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=64;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 b.FillVirtual.FillSizeInBytes=4;b.FillVirtual.FillPattern=0x99999999;b.DmaBufferGpuVirtualAddress=~0ull-8;
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
       PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,&read)==PAGING_PT_OK && read==0x1234567812345678ull,"virtual fill publication refusal preserves logical contents and progress");
 b.DmaBufferGpuVirtualAddress=0;translationOk=0;
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset,"virtual fill translation refusal is not empty success");
 translationOk=1;
 check(GfxPagingBuildFillPage(&d,4096,0x40004,4096,0,dma,0,sizeof(dma),&written,&physical,&system)==STATUS_INVALID_PARAMETER && !written,"fill-page builder rejects crossing a VA page");
 translated=(u64)d.VramPhysical.QuadPart+app;
 b.FillVirtual.DestinationVirtualAddress=0x40004;b.FillVirtual.FillSizeInBytes=(1ull<<32)+8192;
 b.MultipassOffset=1u<<20;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
 b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildVirtualFill(&d,4096,&b,&moved)==STATUS_SUCCESS && moved==8196 &&
       b.MultipassOffset==(1u<<20)+3 && b.pDmaBuffer==dma+15,
       "virtual Fill resumes above4GiB with exact three-slice tail and no length truncation");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,60,0,NULL,NULL),
       "wide virtual Fill publishes all tail records with contiguous offsets");

 check(GfxPagingBuildFillPage(&d,4096,0x40004,4,0,dma,0,sizeof(dma),&written,&physical,&system)==STATUS_SUCCESS &&
       written==5 && physical==translated+4 && !system,"fill-page builder returns exact local CPU physical identity");
 {
  static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
  struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 scratch[16];
  hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
  g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
  gfx.PagingWindowReady=1;
  check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"virtual fill system aperture initialized");
  isSystem=1;translated=0x12345000;
  check(GfxPagingBuildFillPage(&d,4096,0x40004,4,0x11223344,dma,128,sizeof(dma),&written,&physical,&system)==STATUS_SUCCESS &&
        written==81 && physical==0x12345004 && system && dma[36]==0x11223344,"fill-page builder returns exact system identity with GART fill packets");
  memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;isSystem=0;
 }
 translation_by_offset=0;layout_fb_known=1;memset(&g_VidMm,0,sizeof(g_VidMm));
}

static void case_virtual_fill_real_walk(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[64],priv[96];
 u64 app,appLen,table,tableLen,base,root,pa,moved,encoded;BOOLEAN system;
 unsigned i;NTSTATUS status;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramLength=16ull<<30;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&appLen,&table,&tableLen),"fill real-walker advertised split layout");
 cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
 check(VidMmStartLayout(&d,app,appLen,1,table,tableLen,3)==STATUS_SUCCESS,"fill real-walker startup");
 base=(u64)d.VramPhysical.QuadPart+table;root=base+4096;
 memset(cpu_storage,0,sizeof(cpu_storage));
 pte.Flags=BC250_DXGK_PTE_VALID | (3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i+1;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"fill real-walker directory initialization");
 }
 u.PageTableLevel=0;u.PageTableAddress.CpuVirtual=cpu_storage+4*512;
 u.StartIndex=16;pte.PageAddress=4;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"fill first VA maps its own leaf table");
 u.StartIndex=17;pte.PageAddress=5;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"fill second VA initially maps another page");
 encoded=cpu_storage[4*512+17];use_retained_walk=1;
 check(VidMmTranslatePaging(root,0x10000,&pa,&system) && !system && pa==base+4*4096,"logical first destination positive control");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && !system && pa==base+5*4096,"logical second destination positive control");
 check(VidMmTranslateRetained(root,0x11000,&pa,&system) && pa==base+5*4096,"live second destination positive control");
 b.FillVirtual.DestinationVirtualAddress=0x10000;b.FillVirtual.FillSizeInBytes=8192;b.FillVirtual.FillPattern=0;
 b.pDmaBuffer=dma;b.DmaSize=0;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);b.DmaBufferWriteOffset=64;
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildVirtualFill(&d,root,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset,
       "no command capacity prevents self-table fill publication");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && pa==base+5*4096 && all_pattern(dma,64,0xCCCCCCCCu),
       "unpublished fill leaves actual logical mapping and DMA untouched");
 b.DmaSize=sizeof(dma);
 status=WddmBuildVirtualFill(&d,root,&b,&moved);
 check(status==STATUS_INVALID_PARAMETER && moved==4096 && b.MultipassOffset==1,
       "accepted self-table fill makes next real translation fail after one-page prefix");
 check(b.pDmaBuffer==dma+5 && b.DmaSize==sizeof(dma)-20 && b.DmaBufferWriteOffset==64,
       "later translation refusal preserves first fill command and restores input offset");
 check((((u64)dma[2]<<32)|dma[1])==d.VramMcBase+table+4*4096 && dma[3]==0 && dma[4]==4095,
       "self-table fill packet retains resolved first physical target");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,20,0,NULL,NULL),
       "accepted self-table fill private record covers prefix exactly");
 check(!VidMmTranslatePaging(root,0x11000,&pa,&system),"actual logical walker sees accepted PTE invalidation");
 check(VidMmTranslateRetained(root,0x11000,&pa,&system) && pa==base+5*4096 && cpu_storage[4*512+17]==encoded,
       "construction leaves live table unchanged before GPU execution");
 // Independent host application of the decoded first command, not GPU execution.
 {
  u64 mc=((u64)dma[2]<<32)|dma[1],offset=mc-d.VramMcBase-table;
  unsigned bytes=dma[4]+1;
  check(offset==4*4096 && bytes==4096,"decoded self-table fill bounds fit host memory model");
  if(offset==4*4096 && bytes==4096)memset((unsigned char*)cpu_storage+(size_t)offset,0,bytes);
 }
 check(!VidMmTranslateRetained(root,0x11000,&pa,&system),"live walker observes invalidation after modeled fill execution");
 use_retained_walk=0;layout_fb_known=1;VidMmStop();
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,
       "real-walker fill test drains storage and balances locks");
}

static void case_prepare_transfer(void)
{
 BC250_DEVICE d={0};DXGKARG_BUILDPAGINGBUFFER b={0};BC250_PAGING_ENDPOINT source,destination;
 struct {MDL mdl;PFN_NUMBER pages[4];} m={{4*4096,0},{0x123,0x789,0x456,0xABC}};
 struct {MDL mdl;PFN_NUMBER pages[4];} n={{4*4096,0},{0xFED,0x987,0x654,0x321}};
 u64 app,len,table,tableLen,progress,address;unsigned flag;
 d.VramEnabled=1;d.VramLength=16ull<<30;d.VramMcBase=0x100000000ull;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&len,&table,&tableLen),"Transfer preparation shared segment layout");
 b.Transfer.TransferSize=8192;b.Transfer.TransferOffset=28;b.Transfer.MdlOffset=1;
 b.Transfer.Source.SegmentId=0;b.Transfer.Source.pMdl=&m.mdl;
 b.Transfer.Destination.SegmentId=1;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+64);
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && !progress && source.Mdl==&m.mdl &&
       !source.Address && source.FirstPage==1 && destination.Address==d.VramMcBase+app+92 && !destination.Mdl,
       "MDL-to-local preparation separates page offset from segment byte offset");
 check(GfxPagingMdlAddress(source.Mdl,source.FirstPage,0,4,&address) && address==m.pages[1]*4096,
       "TransferOffset is not added to prepared MDL address");
 b.MultipassOffset=1;
 for(flag=0;flag<32;flag++) {
  BOOLEAN ok;b.Transfer.Flags.Value=flag;
  ok=WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress);
  check((flag&3)?!ok && !source.Mdl && !source.Length && !destination.Length && !progress:
        ok && progress==4004,"Transfer flags preserve slice resume and refuse swizzle variants");
 }
 b.Transfer.Flags.Value=0x80000000u;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && !source.Length && !destination.Length && !progress,
       "reserved Transfer flag refuses without prepared output");
 b.Transfer.Flags.Value=28;b.Transfer.Destination.SegmentId=0;b.Transfer.Destination.pMdl=&n.mdl;
 b.Transfer.TransferOffset=0xffffffffu;
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && progress==4096 &&
       source.Mdl==&m.mdl && destination.Mdl==&n.mdl && source.FirstPage==1 && destination.FirstPage==1,
       "two distinct MDLs ignore segment-only offset and repeated start/end flags");
 b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+64);b.Transfer.TransferOffset=28;
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && source.Address==d.VramMcBase+app+92 &&
       destination.Mdl==&n.mdl && progress==4004,"local-to-MDL applies byte offset only to source");
 b.Transfer.Destination.SegmentId=3;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+table+128);
 check(WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && progress==3940 &&
       destination.Address==d.VramMcBase+table+156,"local pair uses independent page phases and already-MC bases");
 b.Transfer.Destination.SegmentId=2;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress) && !source.Length && !destination.Length,
       "unimplemented aperture endpoint refuses instead of pretending it is VRAM");
 b.Transfer.Destination.SegmentId=0;b.Transfer.Destination.pMdl=&n.mdl;b.Transfer.MdlOffset=3;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress),"MDL whole transfer beyond described tail refuses");
 b.Transfer.MdlOffset=1;b.Transfer.Destination.pMdl=NULL;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress),"NULL system MDL refuses rather than selecting local address zero");
 b.Transfer.Destination.pMdl=&n.mdl;b.MultipassOffset=100;
 check(!WddmPreparePhysicalTransfer(&d,&b,&source,&destination,&progress),"out-of-range Transfer slice token refuses");
 layout_fb_known=1;
}

static void case_copy_slice_identity(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 scratch[16];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT src={0},dst={0};BC250_PAGING_COPY_SLICE slice;
 struct {MDL mdl;PFN_NUMBER pages[3];} m={{3*4096,0},{0x123,0x987,0x456}};
 struct {MDL mdl;PFN_NUMBER pages[3];} n={{3*4096,0},{0xABC,0xDEF,0x321}};
 u32 buffer[256];ULONG written;NTSTATUS status;
 d.Gfx=&gfx;d.VramMcBase=0x100000000ull;d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=1ull<<20;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;gfx.PagingWindowReady=1;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"copy slice aperture initialized");
 src.Address=d.VramMcBase+1;src.Length=4096;dst.Address=d.VramMcBase+3;dst.Length=4096;
 status=GfxPagingBuildCopyPage(&d,&src,&dst,0,17,buffer,128,sizeof(buffer),&written,&slice);
 check(status==STATUS_SUCCESS && written==34 && slice.Bytes==17 && !slice.SourceSystem && !slice.DestinationSystem &&
       slice.SourcePhysical==0x200000001ull && slice.DestinationPhysical==0x200000003ull,"local overlap slice returns identities used by staged packet");
 check(buffer[3]==1 && buffer[5]==0x80000 && buffer[20]==0x80000 && buffer[22]==3 && buffer[10]==97 && buffer[27]==98,
       "copy slice uses reserved stage and command-position markers");
 memset(buffer,0xCC,sizeof(buffer));
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,buffer,128,47*4,&written,&slice)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
       !written && !slice.Bytes && !slice.SourcePhysical && !slice.DestinationPhysical && all_pattern(buffer,256,0xCCCCCCCCu),"short staged slice clears identities and emits nothing");
 gfx.PagingCopyStaging.size=0;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER && !slice.Bytes,"missing stage refuses overlap");
 dst.Address=d.VramMcBase+8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==7 &&
       slice.SourcePhysical==0x200000005ull && slice.DestinationPhysical==0x200002004ull,"disjoint local copy keeps direct path and progress-adjusted identities");
 src.Mdl=&m.mdl;src.FirstPage=1;src.Address=0;src.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==83 &&
       slice.SourceSystem && !slice.DestinationSystem && slice.SourcePhysical==0x987004 && slice.DestinationPhysical==0x200002004ull,
       "MDL source identity reflects FirstPage plus byte progress");
 dst.Mdl=&n.mdl;dst.FirstPage=0;dst.Address=0;dst.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4096,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==83 &&
       slice.SourceSystem && slice.DestinationSystem && slice.SourcePhysical==0x456000 && slice.DestinationPhysical==0xDEF000,
       "two fragmented MDLs return distinct captured PFNs at next page");
 src.Mdl=NULL;src.Address=d.VramMcBase+1;src.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==83 &&
       !slice.SourceSystem && slice.DestinationSystem && slice.SourcePhysical==0x200000005ull && slice.DestinationPhysical==0xABC004,
       "local-to-MDL slice returns direct source and system destination identities");
 src.Mdl=&m.mdl;src.Address=0;
 n.pages[1]=m.pages[2];gfx.PagingCopyStaging.size=4096;memset(buffer,0xCC,sizeof(buffer));
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4096,17,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS && written==100 && slice.SourcePhysical==slice.DestinationPhysical &&
       slice.SourceSystem && slice.DestinationSystem && buffer[38]==0x80000 && buffer[53]==0x80000,
       "physical system alias stages through private VRAM while GART mappings stay active");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4095,2,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER && !written,"copy slice cannot cross either page boundary");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,~0ull,2,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER && !written,"copy slice byte-progress overflow refuses");
 translationOk=1;translation_by_offset=1;translated=0x200000000ull;isSystem=0;fragmented=0;
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,128,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==34 && slice.SourcePhysical==0x200000001ull && slice.DestinationPhysical==0x200000003ull &&
       !slice.SourceSystem && !slice.DestinationSystem && slice.Bytes==17,
       "different virtual pages sharing local physical page use staged captured identities");
 check(buffer[3]==1 && buffer[22]==3,"virtual local alias packet agrees with returned identity");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50080,17,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==7 && slice.DestinationPhysical==0x200000080ull && buffer[5]==0x80,
       "virtual disjoint local ranges retain direct copy");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50080,17,TRUE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==34,"virtual caller can force snapshot for whole-transfer ordering");
 isSystem=1;translated=0x123000;
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,128,sizeof(buffer),&written,&slice)==STATUS_SUCCESS &&
       written==100 && slice.SourceSystem && slice.DestinationSystem &&
       slice.SourcePhysical==0x123001 && slice.DestinationPhysical==0x123003,
       "virtual system alias captures PFNs while mapped staging preserves source");
 memset(buffer,0xCC,sizeof(buffer));
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,0,111*4,&written,&slice)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER &&
       !written && !slice.Bytes && !slice.SourcePhysical && all_pattern(buffer,256,0xCCCCCCCCu),
       "virtual mapped staging refusal publishes no identity or payload");
 check(GfxPagingBuildVirtualCopyPage(&d,0,0x40001,0x50003,17,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy requires paging root");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40FFF,0x50003,2,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy bounds source page");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50FFF,2,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy bounds destination page");
 check(GfxPagingBuildVirtualCopyPage(&d,4096,~0ull,0x50003,1,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual copy rejects address wrap");
 translationOk=0;
 check(GfxPagingBuildVirtualCopyPage(&d,4096,0x40001,0x50003,17,FALSE,buffer,0,sizeof(buffer),&written,&slice)==STATUS_INVALID_PARAMETER &&
       !written && !slice.Bytes,"virtual unresolved copy cannot report stale identities");
 translationOk=1;translation_by_offset=0;isSystem=0;translated=0;
 check(!flush_lock_depth && !flush_region_depth,"copy slice lifetime lock balanced on success and refusal");
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_transfer_publication(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[256],priv[300];static u64 scratch[16];
 static unsigned char memory[32768],expected[32768];
 u64 app,len,table,tableLen,moved,total=0,value=0x123456789ABCDEF0ull,zero=0,read;
 unsigned pass=0,i;NTSTATUS status;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramLength=16ull<<30;d.VramMcBase=0x100000000ull;d.VramPhysical.QuadPart=0x200000000ll;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+24576;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(WddmMemoryLayout(&d,&app,&len,&table,&tableLen),"Transfer publication advertised layout");
 b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+1);
 b.Transfer.Destination.SegmentId=1;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app+4097);
 b.Transfer.TransferSize=8192;b.DmaBufferWriteOffset=64;
 for(i=0;i<sizeof(memory);i++)memory[i]=(unsigned char)(i*13+i/129);
 memcpy(expected,memory,sizeof(memory));memmove(expected+4097,expected+1,8192);
 do {
  b.pDmaBuffer=dma;b.DmaSize=48*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildPhysicalTransfer(&d,&b,&moved);
  check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"physical Transfer accepts staged overlap prefix");
  check(moved==(pass==0?1u:pass==1?4096u:4095u) && b.MultipassOffset==pass+1 && b.DmaBufferWriteOffset==64,
        "physical overlap resumes in reverse slice order with exact progress");
  check(b.pDmaBuffer==dma+34 && dma[3]==(pass==0?8192u:pass==1?4096u:1u) &&
        dma[22]==dma[3]+4096 && dma[5]==24576 && dma[20]==24576,"reverse Transfer stages even individually disjoint slices");
  check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,34*4,0,NULL,NULL),
        "Transfer private record exactly covers accepted command prefix");
  if(dma[3]<16384 && dma[22]<16384 && dma[1]<4096 && dma[18]<4096) {
   memcpy(memory+dma[5],memory+dma[3],dma[1]+1);
   memcpy(memory+dma[22],memory+dma[20],dma[18]+1);
  }
  total+=moved;pass++;
 }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<5);
 check(total==8192 && pass==3 && memcmp(memory,expected,16384)==0,"decoded reverse multi-buffer Transfer matches whole-range memmove");
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=1;g_VidMm.Write=1;
 g_VidMm.SegmentPhysical=(u64)d.VramPhysical.QuadPart+table;g_VidMm.SegmentLength=tableLen;
 check(PagingPtShadowInit(&g_VidMm.Shadow,route_shadow_slots,4)==PAGING_PT_OK,"Transfer table shadow ready");
 check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical,0,1,&value,1)==PAGING_PT_OK,"Transfer source metadata registered");
 check(PagingPtShadowApply(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,1,&zero,1)==PAGING_PT_OK,"Transfer destination metadata registered");
 memset(&b,0,sizeof(b));b.Transfer.Source.SegmentId=3;b.Transfer.Destination.SegmentId=3;
 b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+table);
 b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+table+4096);
 b.Transfer.TransferOffset=1;b.Transfer.TransferSize=7;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=PAGING_PRIVATE_HEADER_BYTES;
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset &&
       PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,&read)==PAGING_PT_OK && !read,
       "refused Transfer capacity leaves logical destination unchanged");
 b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==7 && b.MultipassOffset==1 &&
       PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,&read)==PAGING_PT_OK && read==(value&~255ull),
       "published unaligned table Transfer preserves untouched byte and commits source knowledge");
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 g_VidMm.Ready=0;memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_DEVICE_NOT_READY && !moved && !b.MultipassOffset &&
       !priv[0] && b.pDmaBuffer==dma && all_pattern(dma,256,0xCCCCCCCCu),
       "Transfer shadow commit refusal invalidates record without DMA publication");
 g_VidMm.Ready=1;
 {
  static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
  struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
  struct {MDL mdl;PFN_NUMBER pages[3];} mdl={{3*4096,0},{0x123,0x789,0x456}};
  struct {MDL mdl;PFN_NUMBER pages[3];} other={{3*4096,0},{0xABC,0xDEF,0x321}};
  hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
  gfx.PagingWindowReady=1;
  check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"Transfer publication GART window initialized");
  b.Transfer.Source.SegmentId=0;b.Transfer.Source.pMdl=&mdl.mdl;
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==7 &&
        PagingPtShadowRead(&g_VidMm.Shadow,g_VidMm.SegmentPhysical+4096,0,&read)==PAGING_PT_MISSING,
        "MDL-to-table Transfer clears unknown external bytes instead of retaining stale PTE");
  memset(&b,0,sizeof(b));b.Transfer.Source.pMdl=&mdl.mdl;b.Transfer.Destination.SegmentId=1;
  b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app);
  b.Transfer.TransferOffset=28;b.Transfer.TransferSize=8192;pass=0;total=0;
  do {
   b.pDmaBuffer=dma;b.DmaSize=96*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check(status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER,"MDL-to-local Transfer publishes mapped slice");
   check(moved==(pass%2?28u:4068u) && b.MultipassOffset==pass+1 && b.pDmaBuffer==dma+83,
         "mixed Transfer uses both endpoint page boundaries and resumes exactly");
   total+=moved;pass++;
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<6);
  check(total==8192 && pass==4,"fragmented MDL-to-local transfer covers all bytes in four batches");
  memset(&b,0,sizeof(b));b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+app);
  b.Transfer.Destination.pMdl=&mdl.mdl;b.Transfer.TransferOffset=28;b.Transfer.TransferSize=17;
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==17 && b.MultipassOffset==1 &&
        dma[36]==(u32)(d.VramMcBase+app+28) && dma[38]==(u32)(gfx.PagingWindow.mc+4096),
        "local-to-MDL Transfer publishes exact local source and mapped destination");
  b.MultipassOffset=0;b.Transfer.Source.SegmentId=0;b.Transfer.Source.pMdl=&other.mdl;
  b.Transfer.TransferSize=8192;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  memset(dma,0xCC,sizeof(dma));
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
        "disjoint fragmented MDLs publish complete multi-page transfer");
  other.pages[0]=mdl.pages[1];other.pages[1]=mdl.pages[0];
  b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  memset(dma,0xCC,sizeof(dma));
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset && all_pattern(dma,256,0xCCCCCCCCu),
        "swapped physical-page cycle requests a larger buffer without emitting a prefix");
  memset(hub,0,sizeof(*hub));
 }
 memset(&g_VidMm,0,sizeof(g_VidMm));layout_fb_known=1;g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_virtual_transfer_real_walk(void)
{
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE u={0};DXGK_PTE pte={0},zeros[512]={0};
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[128],priv[180];static u64 scratch[16];
 u64 app,appLen,table,tableLen,base,root,pa,moved,encoded;BOOLEAN system;
 unsigned i;NTSTATUS status;
 d.FullWddm=1;d.VramEnabled=1;d.VramWriteEnabled=1;d.VramLength=16ull<<30;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramMcBase=0x100000000ull;d.Gfx=&gfx;
 gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;layout_fb_known=0;
 check(WddmMemoryLayout(&d,&app,&appLen,&table,&tableLen),"Transfer real-walker advertised layout");
 cpu_map_fail=0;cpu_write_setting=1;override_cpu_physical=0;
 check(VidMmStartLayout(&d,app,appLen,1,table,tableLen,3)==STATUS_SUCCESS,"Transfer real-walker startup");
 base=(u64)d.VramPhysical.QuadPart+table;root=base+4096;
 memset(cpu_storage,0,sizeof(cpu_storage));
 pte.Flags=BC250_DXGK_PTE_VALID | (3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
 u.UpdateMode=DXGK_PAGETABLEUPDATE_CPU_VIRTUAL;u.NumPageTableEntries=1;u.pPageTableEntries=&pte;
 for(i=1;i<4;i++) {
  u.PageTableLevel=4-i;u.PageTableAddress.CpuVirtual=cpu_storage+i*512;pte.PageAddress=i+1;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer directory initialization");
 }
 u.PageTableLevel=0;u.PageTableAddress.CpuVirtual=cpu_storage+4*512;
 u.StartIndex=16;pte.PageAddress=4;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer first destination maps leaf table");
 u.StartIndex=17;pte.PageAddress=5;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer second destination maps data page");
 u.StartIndex=32;pte.PageAddress=6;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer source maps known zero page");
 u.StartIndex=33;pte.PageAddress=6;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer next source maps known zero page");
 u.StartIndex=48;pte.PageAddress=5;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer multipass first destination");
 u.StartIndex=49;pte.PageAddress=7;
 check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer multipass second destination");
 u.StartIndex=0;u.NumPageTableEntries=512;u.pPageTableEntries=zeros;
 for(i=5;i<=7;i++) {
  u.PageTableAddress.CpuVirtual=cpu_storage+i*512;
  check(VidMmUpdatePageTable(&u)==STATUS_SUCCESS,"Transfer data page registered with known bytes");
 }
 encoded=cpu_storage[4*512+17];use_retained_walk=1;
 check(VidMmTranslatePaging(root,0x20000,&pa,&system) && !system && pa==base+6*4096,
       "Transfer source actual walker positive control");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && !system && pa==base+5*4096,
       "Transfer destination actual walker positive control");
 b.TransferVirtual.SourceVirtualAddress=0x20000;b.TransferVirtual.DestinationVirtualAddress=0x30000;
 b.TransferVirtual.TransferSizeInBytes=8192;b.DmaBufferWriteOffset=64;
 for(i=0;i<2;i++) {
  b.pDmaBuffer=dma;b.DmaSize=16*4;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildVirtualTransfer(&d,root,&b,&moved);
  check(status==(i?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && moved==4096 &&
        b.MultipassOffset==i+1,"virtual Transfer resumes exactly one page per DMA buffer");
  check(b.pDmaBuffer==dma+7 && b.DmaBufferWriteOffset==64 &&
        PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,28,0,NULL,NULL),
        "virtual Transfer private records cover exact published command");
  check((((u64)dma[4]<<32)|dma[3])==d.VramMcBase+table+6*4096 &&
        (((u64)dma[6]<<32)|dma[5])==d.VramMcBase+table+(i?7:5)*4096,
        "virtual Transfer packet follows independent real page translations");
 }
 b.TransferVirtual.DestinationVirtualAddress=0x10000;b.MultipassOffset=0;
 b.pDmaBuffer=dma;b.DmaSize=0;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildVirtualTransfer(&d,root,&b,&moved)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset,
       "no capacity prevents virtual self-table copy");
 check(VidMmTranslatePaging(root,0x11000,&pa,&system) && pa==base+5*4096 && all_pattern(dma,128,0xCCCCCCCCu),
       "unpublished copy preserves actual logical mapping and DMA");
 b.DmaSize=sizeof(dma);b.DmaBufferGpuVirtualAddress=~0ull-8;
 check(WddmBuildVirtualTransfer(&d,root,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
       VidMmTranslatePaging(root,0x11000,&pa,&system) && pa==base+5*4096,
       "private-header refusal precedes virtual table commit");
 b.DmaBufferGpuVirtualAddress=0;
 status=WddmBuildVirtualTransfer(&d,root,&b,&moved);
 check(status==STATUS_INVALID_PARAMETER && moved==4096 && b.MultipassOffset==1,
       "accepted virtual self-table copy invalidates next real translation after one prefix");
 check(b.pDmaBuffer==dma+7 && b.DmaSize==sizeof(dma)-28 && b.DmaBufferWriteOffset==64 &&
       PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,28,0,NULL,NULL),
       "later translation refusal preserves published copy and restores input offset");
 check(!VidMmTranslatePaging(root,0x11000,&pa,&system),"logical walker observes accepted copy before next construction");
 check(VidMmTranslateRetained(root,0x11000,&pa,&system) && pa==base+5*4096 && cpu_storage[4*512+17]==encoded,
       "copy construction leaves live table untouched");
 {
  u64 src=(((u64)dma[4]<<32)|dma[3])-d.VramMcBase-table;
  u64 dst=(((u64)dma[6]<<32)|dma[5])-d.VramMcBase-table;
  unsigned bytes=dma[1]+1;
  check(src==6*4096 && dst==4*4096 && bytes==4096,"decoded copy fits independent host data model");
  if(src==6*4096 && dst==4*4096 && bytes==4096)
   memcpy((unsigned char*)cpu_storage+(size_t)dst,(unsigned char*)cpu_storage+(size_t)src,bytes);
 }
 check(!VidMmTranslateRetained(root,0x11000,&pa,&system),"live walker observes copied invalidation after modeled execution");
 use_retained_walk=0;VidMmStop();
 // Resume near the end of a wide request without simulating a4GiB GPU run.
 translated=(u64)d.VramPhysical.QuadPart+app;translation_by_offset=1;translationOk=1;isSystem=0;
 gfx.PagingCopyStaging.mc=d.VramMcBase+app+0x80000;gfx.PagingCopyStaging.size=4096;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 memset(&b,0,sizeof(b));b.TransferVirtual.SourceVirtualAddress=0x20000;
 b.TransferVirtual.DestinationVirtualAddress=0x30000;b.TransferVirtual.TransferSizeInBytes=(1ull<<32)+17;
 b.MultipassOffset=1u<<20;b.DmaBufferWriteOffset=128;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
 b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildVirtualTransfer(&d,4096,&b,&moved)==STATUS_SUCCESS && moved==17 &&
       b.MultipassOffset==(1u<<20)+1 && b.pDmaBuffer==dma+34,
       "virtual Transfer resumes beyond4GiB with exact staged tail");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,136,0,NULL,NULL),
       "wide virtual Transfer tail has valid private coverage");
 translation_by_offset=0;layout_fb_known=1;g_adev.sdma.fence_mem.cpu=NULL;
 check(!cpu_map_live && !shadow_pool_live && !cpu_lock_depth && !flush_lock_depth && !flush_region_depth,
       "real-walker Transfer test drains storage and balances locks");
}

static void case_aperture_partition(void)
{
 PAGING_WINDOW window;PAGING_APERTURE aperture,invalid;
 unsigned long long mc,table;unsigned i;
 const unsigned long long start=0x300000000ull,root=0x400000000ull,limit=0x1000000000000ull;
 check(PagingWindowInit(start,512ull<<20,root,1ull<<20,&window),"OS partition temporary window positive control");
 check(PagingApertureInit(start,512ull<<20,root,1ull<<20,&aperture),"OS aperture fits existing512MiB GART");
 check(aperture.mc==start+67117056 && aperture.table==root+131088 &&
       aperture.bytes==268435456,"OS aperture independently calculated MC and PTE extent");
 check(aperture.mc>=window.mc+8192 && aperture.table>=window.table+16 &&
       aperture.mc>=start+67108864,"OS partition excludes driver allocations and both temporary slots");
 check(PagingApertureRange(&aperture,0,65536,&mc,&table) &&
       mc==aperture.mc && table==aperture.table,"complete advertised OS range is representable");
 check(PagingApertureRange(&aperture,65535,1,&mc,&table) &&
       mc==start+335548416 && table==root+655368,"last OS page maps final reserved PTE");
 for(i=0;i<65536;i+=997) {
  check(PagingApertureRange(&aperture,i,65536-i,&mc,&table) &&
        mc==start+67117056+(unsigned long long)i*4096 &&
        table==root+131088+(unsigned long long)i*8,
        "OS range coordinates remain independent across aperture");
 }
 check(!PagingApertureRange(&aperture,65535,2,&mc,&table) && !mc && !table,"OS page count cannot overrun final PTE");
 check(!PagingApertureRange(&aperture,65536,1,&mc,&table) && !mc && !table,"OS first page cannot equal exclusive end");
 check(!PagingApertureRange(&aperture,0,0,&mc,&table) && !mc && !table,"empty aperture operation refused");
 check(!PagingApertureRange(&aperture,~0ull,1,&mc,&table) && !mc && !table,"OS page offset wrap refused");
 check(!PagingApertureRange(&aperture,1,~0ull,&mc,&table) && !mc && !table,"OS page count wrap refused");
 check(!PagingApertureRange(&aperture,0,1,&mc,&mc) && !mc,"aliased output locations refused");
 check(!PagingApertureInit(start,335552511,root,1ull<<20,&invalid) &&
       !invalid.mc && !invalid.table && !invalid.bytes,"short GART cannot partially advertise OS extent");
 check(!PagingApertureInit(start,512ull<<20,root,655375,&invalid) &&
       !invalid.bytes,"short PTE table cannot back entire OS extent");
 check(!PagingApertureInit(start+1,512ull<<20,root,1ull<<20,&invalid) && !invalid.bytes,"unaligned GART base refused");
 check(!PagingApertureInit(start,512ull<<20,root+1,1ull<<20,&invalid) && !invalid.bytes,"unaligned PTE base refused");
 check(PagingApertureInit(limit-335552512,335552512,limit-655376,655376,&invalid) &&
       invalid.mc+invalid.bytes==limit &&
       invalid.table+(invalid.bytes/4096)*8==limit,"48bit exclusive ends accepted exactly");
 check(!PagingApertureInit(limit-335552512+4096,335552512,root,1ull<<20,&invalid) &&
       !invalid.bytes,"48bit MC overrun refused");
 check(!PagingApertureInit(start,512ull<<20,limit-655376+8,655376,&invalid) &&
       !invalid.bytes,"48bit PTE overrun refused");
 invalid=aperture;invalid.bytes=0;
 check(!PagingApertureRange(&invalid,0,1,&mc,&table) && !mc && !table,"uninitialized OS extent refused");
}

static void case_aperture_ddi(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};struct amdgpu_bo bo={0};
 struct {MDL mdl;PFN_NUMBER pages[520];} m;
 DXGKARG_BUILDPAGINGBUFFER b={0};u32 dma[2048],priv[2080];static u64 scratch[16];
 static u64 logical[PAGING_APERTURE_BYTES/4096];
 u64 table,mc,value;unsigned i,pass,count,done;NTSTATUS status;
 d.Gfx=&gfx;gfx.PagingReady=1;gfx.PagingRing=&ring;gfx.PagingDevicePtr=&g_adev;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.gmc.gart_start=0x300000000ull;g_adev.gmc.gart_size=512ull<<20;
 bo.gpu_addr=0x400000000ull;g_adev.gart.bo=&bo;g_adev.gart.table_size=1ull<<20;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x500000000ull;
 check(PagingApertureInit(g_adev.gmc.gart_start,g_adev.gmc.gart_size,bo.gpu_addr,g_adev.gart.table_size,&d.WddmAperture),
       "aperture DDI uses captured advertised geometry");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,(unsigned)(PAGING_APERTURE_BYTES/4096)),"logical aperture fixture");
 g_VidMm.Ready=TRUE;g_VidMm.Write=TRUE;b.Operation=DXGK_OPERATION_MAP_APERTURE_SEGMENT;
 // Failed private-header publication must not expose a new planned mapping.
 m.pages[1]=0x12311;
 m.mdl.ByteOffset=7;m.mdl.ByteCount=520*4096-14;
 for(i=0;i<520;i++)m.pages[i]=0x12300+i*17;
 m.pages[0]=~0ull; // skipped MDL page is deliberately unrepresentable
 b.MapApertureSegment.SegmentId=2;b.MapApertureSegment.OffsetInPages=13;
 b.MapApertureSegment.NumberOfPages=12;b.MapApertureSegment.pMdl=&m.mdl;
 b.MapApertureSegment.MdlOffset=1;b.MapApertureSegment.Flags.CacheCoherent=1;b.DmaBufferWriteOffset=64;
 for(pass=0;pass<2;pass++) {
  done=b.MultipassOffset;count=pass?3:9;
  memset(dma,0xCC,sizeof(dma));b.pDmaBuffer=dma;b.DmaSize=48*4;
  b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildAperture(&d,&b,FALSE);
  check(status==(pass?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) &&
        b.MultipassOffset==done+count,"map aperture resumes by accepted PTE count");
  check(PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+(13+done)*4096,4096,&value) && value==m.pages[1+done]*4096,"accepted map publishes actual physical page identity");
  check(PagingApertureRange(&d.WddmAperture,13+done,count,&mc,&table) &&
        (((u64)dma[2]<<32)|dma[1])==table && dma[3]==2*count-1,
        "map packet addresses exact advertised PTE subrange");
  for(i=0;i<count;i++) {
   value=((u64)dma[5+2*i]<<32)|dma[4+2*i];
   check((value&AMDGPU_PTE_ADDR_MASK)==m.pages[1+done+i]*4096 &&
         (value&AMDGPU_PTE_SNOOPED) && (value&AMDGPU_PTE_SYSTEM) && (value&AMDGPU_PTE_VALID),
         "map preserves fragmented MDL PFNs, MdlOffset and coherent flags");
  }
  check(dma[4+2*count+3]==49 && dma[14+2*count+1]==TEST_REQ_ID &&
        dma[14+2*count+12]==1,"map orders fresh marker then GART invalidate");
  check(b.DmaBufferWriteOffset==64 && b.pDmaBuffer==dma+29+2*count &&
        PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),64,(29+2*count)*4,0,NULL,NULL),
        "map publishes matching DMA/private records without changing input command offset");
 }
 b.MapApertureSegment.Flags.CacheCoherent=0;b.MapApertureSegment.NumberOfPages=1;b.MultipassOffset=0;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_SUCCESS && !(dma[4]&AMDGPU_PTE_SNOOPED),
       "noncoherent map clears snoop while retaining explicit PFN");
 b.MapApertureSegment.NumberOfPages=519;b.MultipassOffset=0;
 for(pass=0;pass<3;pass++) {
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  count=pass==2?7:256;
  check(WddmBuildAperture(&d,&b,FALSE)==(pass==2?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) &&
        b.MultipassOffset==(pass==2?519:(pass+1)*256) && b.pDmaBuffer==dma+29+2*count,
        "large MDL map uses bounded stack batches and includes partial final MDL page");
 }
 b.MapApertureSegment.NumberOfPages=1;b.MultipassOffset=0;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));b.DmaBufferGpuVirtualAddress=~0ull-8;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset &&
       b.pDmaBuffer==dma && all_pattern(dma,2048,0xCCCCCCCCu),"aperture header refusal cannot advance mapping progress");
 b.DmaBufferGpuVirtualAddress=0;b.DmaSize=31*4;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !b.MultipassOffset &&
       all_pattern(dma,2048,0xCCCCCCCCu),"aperture insufficient complete reservation writes no DMA");
 b.DmaSize=sizeof(dma);b.MapApertureSegment.Flags.Value=2;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"reserved map flag refused");
 b.MapApertureSegment.Flags.Value=0;b.MapApertureSegment.NumberOfPages=520;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"map preflights whole MDL page span");
 b.MapApertureSegment.NumberOfPages=1;b.MapApertureSegment.OffsetInPages=65536;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"map cannot escape advertised aperture");
 b.MapApertureSegment.OffsetInPages=13;m.pages[1]=~0ull;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset &&
       all_pattern(dma,2048,0xCCCCCCCCu),"unrepresentable PFN cannot become a truncated successful mapping");
 {
  ULONGLONG before=0,after=0;
  b.Operation=DXGK_OPERATION_MAP_APERTURE_SEGMENT;
  b.MapApertureSegment.OffsetInPages=13;b.MapApertureSegment.MdlOffset=1;
  b.MapApertureSegment.NumberOfPages=12;b.MapApertureSegment.pMdl=&m.mdl;
  check(PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+13*4096,4,&before),"old mapping exists before publication refusal");
  m.pages[1]=0x77777;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
  b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=0;
  check(WddmPublishPagingRecord(&b,8,FALSE,0,1)==STATUS_INVALID_PARAMETER &&
        PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+13*4096,4,&after) && before==after,
        "private header refusal leaves aperture mapping unchanged");
  b.DmaBufferPrivateDataSize=sizeof(priv);
 }
 m.pages[1]=0x12311;d.WddmAperture.table+=8;
 check(WddmBuildAperture(&d,&b,FALSE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"live geometry mismatch prevents writes to stale PTE range");
 d.WddmAperture.table-=8;
 memset(&b,0,sizeof(b));b.Operation=DXGK_OPERATION_UNMAP_APERTURE_SEGMENT;b.UnmapApertureSegment.SegmentId=2;b.UnmapApertureSegment.OffsetInPages=65534;
 b.UnmapApertureSegment.NumberOfPages=2;b.UnmapApertureSegment.DummyPage.QuadPart=0x98765000;
 b.DmaBufferWriteOffset=128;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);
 b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 { u64 mapped[2]={0x11111000,0x22222000};
   check(PagingApertureStateMap(&g_VidMm.Aperture,65534,2,mapped,~4095ull),"seed actual mapping before unmap"); }
 check(WddmBuildAperture(&d,&b,TRUE)==STATUS_SUCCESS && b.MultipassOffset==2 &&
       b.pDmaBuffer==dma+33 && (dma[4]&AMDGPU_PTE_VALID) && dma[4]==dma[6] && dma[5]==dma[7] &&
       ((((u64)dma[5]<<32)|dma[4])&AMDGPU_PTE_ADDR_MASK)==0x98765000,
       "unmap final aperture pages repeat supplied valid dummy PTE instead of zero");
 check(PagingPrivateVisit(priv,(unsigned)(sizeof(priv)-b.DmaBufferPrivateDataSize),128,33*4,0,NULL,NULL),
       "unmap publishes ordered packet with exact private coverage");
 b.MultipassOffset=0;b.UnmapApertureSegment.DummyPage.QuadPart++;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildAperture(&d,&b,TRUE)==STATUS_INVALID_PARAMETER && !b.MultipassOffset,"unaligned dummy physical page refused");
 check(!PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc+65534ull*4096,4,&value),"accepted unmap invalidates logical access despite valid hardware dummy PTE");
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 check(!flush_lock_depth && !flush_region_depth,"aperture map/unmap engine lifetime locks balance");
 memset(hub,0,sizeof(*hub));g_adev.gart.bo=NULL;g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_aperture_state_lifetime(void)
{
 BC250_DEVICE d={0};int before=shadow_pool_live;u64 physical;
 d.FullWddm=d.VramEnabled=d.VramWriteEnabled=1;
 d.VramPhysical.QuadPart=0x200000000ull;d.VramLength=0x200000;
 cpu_map_fail=0;cpu_write_setting=1;
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"lifetime aperture geometry");
 check(VidMmStartLayout(&d,0,0x100000,1,0x100000,sizeof(cpu_storage),3)==STATUS_SUCCESS,
       "actual VidMm start prepares table and aperture storage");
 check(shadow_pool_live==before+2 && g_VidMm.Aperture.count==65536 &&
       !PagingApertureStateResolve(&g_VidMm.Aperture,d.WddmAperture.mc,4,&physical),
       "new aperture storage is owned and initially unmapped");
 VidMmStop();
 check(shadow_pool_live==before && !g_VidMm.Aperture.entries && !g_VidMm.Ready,
       "actual VidMm stop releases logical storage under its CPU-reader lock");
}

// Decode actual map/copy packets into independent sparse physical backing.
// This checks byte content across DDI resumes, not GPU ordering/cache behavior.
static void case_indirect_multipass_bytes(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 scratch[16],logical[65536];
 static unsigned char memory[6][4096],expected[6][4096];
 const u64 physical[6]={0x100123000ull,0x300789000ull,0x200456000ull,
                        0x400ABC000ull,0x600DEF000ull,0x500321000ull};
 struct {MDL mdl;PFN_NUMBER pages[3];} source={{12288,0},{0}},destination={{12288,0},{0}};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned mode,limit,i,j,pass;u32 dma[1024],priv[1200];
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"byte replay temporary window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"byte replay permanent aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"byte replay logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 check(PagingApertureStateMap(&g_VidMm.Aperture,0,3,physical,~4095ull) &&
       PagingApertureStateMap(&g_VidMm.Aperture,10,3,physical+3,~4095ull),"byte replay fragmented high physical pages");
 for(i=0;i<3;i++){source.pages[i]=physical[i]>>12;destination.pages[i]=physical[i+3]>>12;}
 for(mode=0;mode<4;mode++)for(limit=0;limit<2;limit++) {
  DXGKARG_BUILDPAGINGBUFFER b={0};u64 moved,total=0;NTSTATUS status;
  unsigned srcStart=(mode&1)?17:0,dstStart=(mode&2)?31:0;
  for(i=0;i<6;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  memcpy(expected,memory,sizeof(memory));
  for(i=0;i<8192;i++)expected[3+(dstStart+i)/4096][(dstStart+i)%4096]=memory[(srcStart+i)/4096][(srcStart+i)%4096];
  if(mode&1){b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+srcStart);}
  else b.Transfer.Source.pMdl=&source.mdl;
  if(mode&2){b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+10*4096+dstStart);}
  else b.Transfer.Destination.pMdl=&destination.mdl;
  b.Transfer.TransferSize=8192;b.DmaBufferWriteOffset=128;
  pass=0;
  do {
   u64 srcPhys,dstPhys,srcMc,dstMc;unsigned sp=6,dp=6,so,doff,bytes,prior=b.MultipassOffset;
   b.pDmaBuffer=dma;b.pDmaBufferPrivateData=priv;
   b.DmaSize=limit?sizeof(dma):96*4;
   b.DmaBufferPrivateDataSize=limit?PAGING_PRIVATE_HEADER_BYTES+96*4:sizeof(priv);
   memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check((status==STATUS_SUCCESS || status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) && moved &&
         b.MultipassOffset==prior+1 && b.DmaBufferWriteOffset==128 && b.pDmaBuffer==dma+83,
         "indirect byte replay accepts exactly one slice with DMA or private capacity limit");
   if(b.pDmaBuffer!=dma+83 || !moved)break;
   check(PagingPrivateVisit(priv,(unsigned)((limit?PAGING_PRIVATE_HEADER_BYTES+96*4:sizeof(priv))-b.DmaBufferPrivateDataSize),128,83*4,0,NULL,NULL),
         "indirect byte replay accepted private record covers exact command range");
   srcPhys=((u64)dma[4]|((u64)dma[5]<<32))&0x0000FFFFFFFFF000ull;
   dstPhys=((u64)dma[6]|((u64)dma[7]<<32))&0x0000FFFFFFFFF000ull;
   srcMc=(u64)dma[36]|((u64)dma[37]<<32);dstMc=(u64)dma[38]|((u64)dma[39]<<32);
   so=(unsigned)(srcMc-gfx.PagingWindow.mc);doff=(unsigned)(dstMc-gfx.PagingWindow.mc-4096);bytes=dma[34]+1;
   for(i=0;i<6;i++){if(srcPhys==physical[i])sp=i;if(dstPhys==physical[i])dp=i;}
   check(dma[0]==SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) && dma[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) &&
         sp<3 && dp>=3 && dp<6 && so<4096 && doff<4096 && bytes<=4096-so && bytes<=4096-doff && bytes==moved,
         "indirect byte replay decodes high PFNs and bounded copy offsets/count");
   if(sp>=3 || dp<3 || dp>=6 || so>=4096 || doff>=4096 || bytes>4096-so || bytes>4096-doff)break;
   memcpy(memory[dp]+doff,memory[sp]+so,bytes);
   check(all_pattern(dma+83,1024-83,0xCCCCCCCCu),"indirect byte replay leaves unused DMA tail untouched");
   total+=moved;pass++;
  }while(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && pass<8);
  check(status==STATUS_SUCCESS && total==8192 && pass>=2 && pass<=5 && !memcmp(memory,expected,sizeof(memory)),
        "MDL/aperture multi-buffer copy matches independent byte oracle including untouched boundaries");
 }
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

static void case_aperture_transfer(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];static u64 scratch[16],logical[65536];
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT src={0},dst={0};BC250_PAGING_COPY_SLICE slice;
 struct {MDL mdl;PFN_NUMBER pages[2];} m={{8192,0},{0x123,0x987}};
 DXGKARG_BUILDPAGINGBUFFER b={0};u64 pages[2]={0x123000,0x987000},progress,moved;
 u32 dma[2048],priv[2080];ULONG written;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;gfx.PagingWindowReady=1;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=scratch;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"transfer temporary window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"transfer permanent aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"transfer logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 check(PagingApertureStateMap(&g_VidMm.Aperture,0,2,pages,~4095ull),"transfer maps fragmented physical pages");
 b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
 b.Transfer.Destination.SegmentId=1;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+0x20000);
 b.Transfer.TransferSize=8192;
 check(WddmPreparePhysicalTransfer(&d,&b,&src,&dst,&progress) && src.Aperture && !src.Mdl && !dst.Aperture,
       "segment2 preflight chooses aperture rather than VRAM base");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,4096,4096,dma,0,sizeof(dma),&written,&slice)==STATUS_SUCCESS &&
       slice.SourceSystem && slice.SourcePhysical==0x987000 && !slice.DestinationSystem,
       "fragmented second aperture page resolves to exact system physical identity");
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
       "actual aperture-to-local DDI publishes both page transactions");
 b.MultipassOffset=0;b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+0x20000);
 b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
       "actual local-to-aperture DDI publishes both page transactions");
 b.MultipassOffset=0;b.Fill.Destination.SegmentId=2;
 b.Fill.Destination.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;b.Fill.FillSize=8192;b.Fill.FillPattern=0xBC250;
 b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 check(WddmBuildPhysicalFill(&d,&b,&moved)==STATUS_SUCCESS && moved==8192,
       "aperture fill resolves both mapped system pages through same endpoint route");
 {u64 otherPages[2]={0xAAA000,0xBBB000};
  check(PagingApertureStateMap(&g_VidMm.Aperture,10,2,otherPages,~4095ull),"map disjoint aperture destination");
  b.MultipassOffset=0;b.Transfer.Source.SegmentId=2;
  b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
  b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+10*4096);
  b.Transfer.TransferSize=8192;
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192 && b.MultipassOffset==2,
        "disjoint fragmented aperture-to-aperture transfer is admitted");
  m.pages[0]=0xAAA;m.pages[1]=0xBBB;
  b.MultipassOffset=0;b.Transfer.Destination.SegmentId=0;b.Transfer.Destination.pMdl=&m.mdl;
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && moved==8192,
        "disjoint fragmented aperture-to-MDL transfer is admitted");
  m.pages[0]=0x123;m.pages[1]=0x987;
 }
 src.Aperture=TRUE;src.Mdl=NULL;src.Address=d.WddmAperture.mc;src.Length=8192;
 dst.Aperture=FALSE;dst.Mdl=&m.mdl;dst.Address=0;dst.FirstPage=0;dst.Length=8192;
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,dma,0,sizeof(dma),&written,&slice)==STATUS_SUCCESS && written==100 &&
       slice.SourceSystem && slice.DestinationSystem && slice.SourcePhysical==slice.DestinationPhysical,
       "aperture/MDL physical alias uses existing VRAM staging transaction");
 pages[0]=0x555000;check(PagingApertureStateMap(&g_VidMm.Aperture,0,1,pages,~4095ull),"publish planned remap");
 check(GfxPagingBuildCopyPage(&d,&src,&dst,0,17,dma,0,sizeof(dma),&written,&slice)==STATUS_SUCCESS &&
       slice.SourcePhysical==0x555000 && slice.DestinationPhysical==0x123000 && written==83,
       "new planned identity used without reading unexecuted hardware GART");
 check(PagingApertureStateUnmap(&g_VidMm.Aperture,1,1),"remove second page");
 b.Transfer.Source.SegmentId=1;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)(d.VramMcBase+0x20000);
 b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;
 b.MultipassOffset=0;b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
 memset(dma,0xCC,sizeof(dma));
 check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_INVALID_PARAMETER && !moved && !b.MultipassOffset &&
       all_pattern(dma,2048,0xCCCCCCCCu),"whole aperture range checked before publishing a mapped prefix");
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

// Replay actual emitted copy packets against an initial byte snapshot. System
// endpoints come from the emitted PTEs; the direct VRAM endpoint is cycle scratch.
static void case_permutation_packets(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 fence[16],logical[65536];
 static unsigned char memory[4][4096],expected[3][4096];
 const u64 physical[3]={0x100123000ull,0x300789000ull,0x200456000ull};
 const unsigned permutations[3][3]={{1,0,2},{1,2,0},{0,1,2}};
 struct {MDL mdl;PFN_NUMBER pages[3];} source={{12288,0},{0}},destination={{12288,0},{0}};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 BC250_PAGING_ENDPOINT src={0},dst={0};
 unsigned mode,plan,i,j,k,nextPage;u32 dma[1024],priv[1050];ULONG written;NTSTATUS status;
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();ring.max_dw=1024;
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"permutation temporary window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"permutation aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"permutation logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 for(plan=0;plan<3;plan++)for(mode=0;mode<4;mode++) {
  u64 destinationPages[3],moved;unsigned actions=plan==0?3:plan==1?4:0;
  DXGKARG_BUILDPAGINGBUFFER b={0};
  for(i=0;i<3;i++) {
   source.pages[i]=physical[i]>>12;
   destinationPages[i]=physical[permutations[plan][i]];
   destination.pages[i]=destinationPages[i]>>12;
  }
  check(PagingApertureStateMap(&g_VidMm.Aperture,0,3,physical,~4095ull) &&
        PagingApertureStateMap(&g_VidMm.Aperture,10,3,destinationPages,~4095ull),"permutation captures source/destination physical identities");
  memset(&src,0,sizeof(src));memset(&dst,0,sizeof(dst));src.Length=dst.Length=12288;
  if(mode&1){src.Aperture=TRUE;src.Address=d.WddmAperture.mc;}else src.Mdl=&source.mdl;
  if(mode&2){dst.Aperture=TRUE;dst.Address=d.WddmAperture.mc+10*4096;}else dst.Mdl=&destination.mdl;
  if(actions) {
   memset(dma,0xCC,sizeof(dma));
   status=GfxPagingBuildPermutation(&d,&src,&dst,12288,dma,128,83*4,0,&written,&nextPage);
   check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !written && all_pattern(dma,1024,0xCCCCCCCCu),
         "whole cycle capacity preflight writes no prefix");
  }
  if(mode&1){b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)src.Address;}
  else b.Transfer.Source.pMdl=src.Mdl;
  if(mode&2){b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)dst.Address;}
  else b.Transfer.Destination.pMdl=dst.Mdl;
  b.Transfer.TransferSize=12288;b.DmaBufferWriteOffset=128;
  if(actions)for(i=0;i<2;i++) {
   b.pDmaBuffer=dma;b.DmaSize=i?sizeof(dma):83*4;
   b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=i?PAGING_PRIVATE_HEADER_BYTES+83*4:sizeof(priv);
   memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check(status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && !moved && !b.MultipassOffset &&
         b.pDmaBuffer==dma && b.pDmaBufferPrivateData==priv && b.DmaBufferWriteOffset==128 &&
         all_pattern(dma,1024,0xCCCCCCCCu),"DDI alias DMA/private shortage publishes no partial cycle or progress");
  }
  for(i=0;i<4;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  for(i=0;i<3;i++)memcpy(expected[permutations[plan][i]],memory[i],4096);
  memset(dma,0xCC,sizeof(dma));
  b.pDmaBuffer=dma;b.DmaSize=sizeof(dma);b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=sizeof(priv);
  status=WddmBuildPhysicalTransfer(&d,&b,&moved);
  written=(ULONG)(((u32*)b.pDmaBuffer)-dma);
  check(status==STATUS_SUCCESS && written==actions*83 && moved==12288 && b.MultipassOffset==3 &&
        b.DmaBufferWriteOffset==128 && b.DmaSize==sizeof(dma)-written*4 &&
        b.DmaBufferPrivateDataSize==sizeof(priv)-(written?PAGING_PRIVATE_HEADER_BYTES+written*4:0),
        "DDI complete permutation advances buffers, sizes and terminal token exactly once");
  if(written)check(PagingPrivateVisit(priv,PAGING_PRIVATE_HEADER_BYTES+written*4,128,written*4,0,NULL,NULL),
                   "DDI cycle private record covers exact published command range");
  if(status!=STATUS_SUCCESS || written!=actions*83)continue;
  for(k=0;k<actions;k++) {
   u32* packet=dma+k*83;
   u64 sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
   u64 dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
   u64 sm=(u64)packet[36]|((u64)packet[37]<<32),dm=(u64)packet[38]|((u64)packet[39]<<32);
   unsigned si=4,di=4;
   if(sm==gfx.PagingCopyStaging.mc)si=3;
   else if(sm==gfx.PagingWindow.mc)for(i=0;i<3;i++)if(sp==physical[i])si=i;
   if(dm==gfx.PagingCopyStaging.mc)di=3;
   else if(dm==gfx.PagingWindow.mc+4096)for(i=0;i<3;i++)if(dp==physical[i])di=i;
   check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && packet[34]==4095 && si<4 && di<4 && si!=di,
         "decode actual SAVE/COPY/RESTORE PTEs and direct scratch address");
   if(si>=4 || di>=4 || si==di)break;
   memcpy(memory[di],memory[si],4096);
  }
  check(k==actions && !memcmp(memory,expected,sizeof(expected)),"emitted cycle preserves initial bytes across MDL/aperture aliases");
  check(all_pattern(dma+written,1024-written,0xCCCCCCCCu),"permutation leaves unused packet storage intact");
  {PVOID afterDma=b.pDmaBuffer,afterPrivate=b.pDmaBufferPrivateData;ULONG left=b.DmaSize,leftPrivate=b.DmaBufferPrivateDataSize;
   check(WddmBuildPhysicalTransfer(&d,&b,&moved)==STATUS_SUCCESS && !moved &&
         b.pDmaBuffer==afterDma && b.pDmaBufferPrivateData==afterPrivate && b.DmaSize==left &&
         b.DmaBufferPrivateDataSize==leftPrivate && b.MultipassOffset==3,
         "completed alias token emits no duplicate cycle and reports no new bytes");}

 }
 check(!flush_lock_depth && !flush_region_depth,"permutation releases engine lifetime ownership");
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));
 memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}

// Three disjoint cycles with noncontiguous members and skipped identity pages.
// DMA, private-record and hardware-ring limits independently force resumes.
static void case_permutation_multipass(unsigned longCycle)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 struct amdgpu_vmhub* hub=&g_adev.vmhub[AMDGPU_GFXHUB(0)];
 static u64 fence[16],logical[65536];
 static unsigned char memory[9][4096],expected[8][4096];
 const unsigned permutations[2][8]={{0,7,2,5,6,3,4,1},{1,2,3,4,5,6,7,0}};
 const unsigned* permutation=permutations[longCycle];
 const unsigned passes=longCycle?7:3;
 u64 physical[8],destinationPages[8];
 struct {MDL mdl;PFN_NUMBER pages[8];} source={{32768,0},{0}},destination={{32768,0},{0}};
 BC250_DEVICE d={0};BC250_GFX gfx={0};struct amdgpu_ring ring={0};
 unsigned mode,limit,i,j,k,pass;u32 dma[2048],priv[2070];
 d.Gfx=&gfx;d.VramEnabled=1;d.VramMcBase=0x100000000ull;
 d.VramPhysical.QuadPart=0x200000000ll;d.VramLength=8ull<<30;
 gfx.PagingReady=gfx.PagingWindowReady=1;gfx.PagingDevicePtr=&g_adev;gfx.PagingRing=&ring;
 gfx.PagingCopyStaging.mc=d.VramMcBase+0x80000;gfx.PagingCopyStaging.size=4096;
 ring.funcs=bc250_sdma_ring_funcs();
 hub->vmhub_funcs=&funcs;hub->vm_inv_eng0_req=TEST_REQ_ID;hub->vm_inv_eng0_ack=TEST_ACK_ID;
 g_adev.sdma.fence_mem.cpu=fence;g_adev.sdma.fence_mem.mc=0x300400000ull;
 check(PagingWindowInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&gfx.PagingWindow),"cycle multipass window");
 check(PagingApertureInit(0x300000000ull,512ull<<20,0x400000000ull,1ull<<20,&d.WddmAperture),"cycle multipass aperture");
 check(PagingApertureStateInit(&g_VidMm.Aperture,&d.WddmAperture,logical,65536),"cycle multipass logical state");
 g_VidMm.Ready=g_VidMm.Write=TRUE;
 for(i=0;i<8;i++)physical[i]=0x100123000ull+(u64)i*0x100003000ull;
 for(i=0;i<8;i++){source.pages[i]=physical[i]>>12;destinationPages[i]=physical[permutation[i]];destination.pages[i]=destinationPages[i]>>12;}
 check(PagingApertureStateMap(&g_VidMm.Aperture,0,8,physical,~4095ull) &&
       PagingApertureStateMap(&g_VidMm.Aperture,10,8,destinationPages,~4095ull),"cycle multipass physical identities");
 for(mode=0;mode<4;mode++)for(limit=longCycle?2:0;limit<3;limit++) {
  DXGKARG_BUILDPAGINGBUFFER b={0};u64 total=0,moved;NTSTATUS status=STATUS_SUCCESS;
  ring.max_dw=limit==2?512:4096;
  if(mode&1){b.Transfer.Source.SegmentId=2;b.Transfer.Source.SegmentAddress.QuadPart=(LONGLONG)d.WddmAperture.mc;}
  else b.Transfer.Source.pMdl=&source.mdl;
  if(mode&2){b.Transfer.Destination.SegmentId=2;b.Transfer.Destination.SegmentAddress.QuadPart=(LONGLONG)(d.WddmAperture.mc+10*4096);}
  else b.Transfer.Destination.pMdl=&destination.mdl;
  b.Transfer.TransferSize=32768;
  for(i=0;i<9;i++)for(j=0;j<4096;j++)memory[i][j]=(unsigned char)(i*43+j*13+j/127);
  for(i=0;i<8;i++)memcpy(expected[permutation[i]],memory[i],4096);
  for(pass=0;pass<passes;pass++) {
   ULONG dmaBytes=limit==0?384*4:sizeof(dma);
   ULONG privateBytes=limit==1?PAGING_PRIVATE_HEADER_BYTES+384*4:sizeof(priv);
   b.pDmaBuffer=dma;b.DmaSize=dmaBytes;b.pDmaBufferPrivateData=priv;b.DmaBufferPrivateDataSize=privateBytes;
   b.DmaBufferWriteOffset=0;memset(dma,0xCC,sizeof(dma));memset(priv,0xCC,sizeof(priv));
   status=WddmBuildPhysicalTransfer(&d,&b,&moved);
   check(status==(pass+1==passes?STATUS_SUCCESS:STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) &&
         b.MultipassOffset==(pass+1==passes?8:PAGING_PERMUTATION_RESUME|((pass+1)*3)) && moved==(pass+1==passes?32768ull:0) &&
         b.pDmaBuffer==dma+249 && b.DmaSize==dmaBytes-249*4 &&
         b.DmaBufferPrivateDataSize==privateBytes-PAGING_PRIVATE_HEADER_BYTES-249*4,
         "each DDI resume publishes exactly one complete cycle and next atomic-group token");
   if(b.pDmaBuffer!=dma+249)break;
   check(PagingPrivateVisit(priv,PAGING_PRIVATE_HEADER_BYTES+249*4,0,249*4,0,NULL,NULL),"multipass cycle private record coverage");
   for(k=0;k<3;k++) {
    u32* packet=dma+k*83;
    u64 sp=((u64)packet[4]|((u64)packet[5]<<32))&0x0000FFFFFFFFF000ull;
    u64 dp=((u64)packet[6]|((u64)packet[7]<<32))&0x0000FFFFFFFFF000ull;
    u64 sm=(u64)packet[36]|((u64)packet[37]<<32),dm=(u64)packet[38]|((u64)packet[39]<<32);
    unsigned si=9,di=9;
    if(sm==gfx.PagingCopyStaging.mc)si=8;
    else if(sm==gfx.PagingWindow.mc)for(i=0;i<8;i++)if(sp==physical[i])si=i;
    if(dm==gfx.PagingCopyStaging.mc)di=8;
    else if(dm==gfx.PagingWindow.mc+4096)for(i=0;i<8;i++)if(dp==physical[i])di=i;
    check(packet[33]==SDMA_PKT_HEADER_OP(SDMA_OP_COPY) && packet[34]==4095 && si<9 && di<9 && si!=di,
          "multipass actual packets decode bounded page copy");
    if(si>=9 || di>=9 || si==di)break;
    memcpy(memory[di],memory[si],4096);
   }
   total+=moved;
   memset(memory[8],0xED,4096); // An unrelated retired paging job reuses scratch.
  }
  check(pass==passes && status==STATUS_SUCCESS && total==32768 && !memcmp(memory,expected,sizeof(expected)),
        "all multipass alias bytes match initial snapshot despite scratch overwrite between jobs");
 }
 RtlZeroMemory(&g_VidMm.Aperture,sizeof(g_VidMm.Aperture));memset(hub,0,sizeof(*hub));g_adev.sdma.fence_mem.cpu=NULL;
}
int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			g_verbose = 1;

	printf("Paging node packets (ADR 0008 stage D): driver/shim/bc250_sdma_paging.c against\n");
	printf("bc250_sdma_emit_copy_linear()/emit_fill() (driver/shim/bc250_sdma_copy.c, M95), dword for\n");
	printf("dword, plus the room check BuildPagingBuffer's STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER needs.\n");

	for (i=1;i<argc;i++) if (strcmp(argv[i], "--queued-pte-ordering")==0) { case_queued_pte_ordering(); printf("queued PTE ordering: %u checks, %u failures\n",g_checks,g_failures); return g_failures ? 1 : 0; }
	case_transfer_three_pages();
	case_fill_two_pages();
	case_insufficient();
	case_refusals();
	case_write_ptes();
	case_ordered_ptes();
	case_aperture_packets();
	case_staged_pte_copy();
	case_staged_byte_copy();
	case_mapped_staging();
	case_gart_invalidate();
	case_mapped_copy();
	case_kmd_routes();
	case_kmd_flush();
	case_kmd_updates();
	case_cpu_updates();
	case_retained_mapping();
	case_queued_pte_ordering();
	case_copy_range_builder();
	case_copy_publication();
	case_copy_real_walk();
	case_separate_table_extent();
	case_wddm_memory_layout();
	case_wddm_segment_queries();
	case_probe_physical_copy();
	case_mdl_address();
	case_physical_stream();
	case_physical_fill_ddi();
	case_virtual_fill_publication();
	case_virtual_fill_real_walk();
	case_prepare_transfer();
	case_copy_slice_identity();
	case_transfer_publication();
	case_virtual_transfer_real_walk();
	case_aperture_partition();
	case_aperture_ddi();
	case_aperture_state_lifetime();
	case_aperture_transfer();
	case_indirect_multipass_bytes();case_permutation_packets();case_permutation_multipass(0);case_permutation_multipass(1);

	printf("\n== verdict ==\n");
	printf("  %u checks, %u failures\n", g_checks, g_failures);
	printf("  %s\n", g_failures == 0 ? "PASS" : "FAIL");
	return g_failures == 0 ? 0 : 1;
}
