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


#include "paging_window.h"
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
typedef struct {struct {long long QuadPart;} VramPhysical;ULONGLONG VramMcBase,VramLength;void*Gfx;int GfxPagingLock;} BC250_DEVICE;
typedef struct {BOOLEAN PagingWindowReady;PAGING_WINDOW PagingWindow;struct amdgpu_device*PagingDevicePtr;BOOLEAN PagingReady;struct amdgpu_ring*PagingRing;BOOLEAN PagingCpuBootstrap;} BC250_GFX;
static ULONGLONG translated;static int isSystem,translationOk=1,fragmented;
static int VidMmTranslate(ULONGLONG r,ULONGLONG v,ULONGLONG*p,BOOLEAN*s){(void)r;(void)v;*p=fragmented ? 0x100000ull+((v/4096)%17)*8192+(v&4095) : translated;*s=isSystem;return translationOk;}

typedef long NTSTATUS;
typedef void* PVOID;
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER (-1)
#define STATUS_DEVICE_NOT_READY (-2)
#define STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER (-3)
#define BC250_GFX_PAGING_BUFFER_BYTES 65536u
static int flush_lock_depth,flush_region_depth;
static void KeEnterCriticalRegion(void){flush_region_depth++;}
static void KeLeaveCriticalRegion(void){flush_region_depth--;}
static void ExAcquirePushLockShared(int*p){(void)p;flush_lock_depth++;}
static void ExReleasePushLockShared(int*p){(void)p;flush_lock_depth--;}

#include "bc250_pte.h"
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define BC250_VIDMM_LEVELS 4u
#define BC250_VIDMM_PTES 512u
#define DXGK_PAGETABLEUPDATE_GPU_PHYSICAL 1
/* Field-level host model, not a WDK layout test; the full KMD build checks ABI. */
typedef struct {ULONG SegmentId;ULONGLONG SegmentOffset;} D3DGPU_PHYSICAL_ADDRESS;
typedef struct {ULONGLONG Flags,PageAddress;} DXGK_PTE;
typedef struct {
 UINT UpdateMode,PageTableLevel,NumPageTableEntries,StartIndex;
 struct {int Use64KBPages,Repeat;} Flags;
 struct {D3DGPU_PHYSICAL_ADDRESS GpuPhysical;} PageTableAddress;
 const DXGK_PTE*pPageTableEntries;
} DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE;
typedef struct {BOOLEAN Ready,Write;ULONGLONG SegmentPhysical,SegmentLength;struct bc250_pte_context Pte;} BC250_VIDMM;
static BC250_VIDMM g_VidMm;
static enum bc250_pte_kind VidMmKind(UINT level){return level ? BC250_PTE_DIRECTORY : BC250_PTE_LEAF;}
static unsigned cpu_pte_calls;
static void VidMmUpdatePageTable(const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE*u){(void)u;cpu_pte_calls++;}
static void KeMemoryBarrier(void){}
BOOLEAN VidMmRootPhysical(_In_ const D3DGPU_PHYSICAL_ADDRESS* Address, _Out_ ULONGLONG* Physical)
{
    const BC250_VIDMM* vm = &g_VidMm;

    *Physical = 0;
    // Plan-only mode (EnableGpuVa closed) has no root: tables nobody wrote are not tables to point a GPU at.
    if (!vm->Ready || !vm->Write || vm->SegmentLength < PAGE_SIZE || Address->SegmentId != vm->Pte.vram_segment ||
        (Address->SegmentOffset & (PAGE_SIZE - 1)) != 0 || Address->SegmentOffset > vm->SegmentLength - PAGE_SIZE)
        return FALSE;
    *Physical = vm->SegmentPhysical + Address->SegmentOffset;
    return TRUE;
}

BOOLEAN VidMmEncodePageTable(_In_ const DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE* Update,
                            ULONG Start, ULONG Count, _Out_ ULONGLONG* Physical,
                            _Out_writes_(Count) ULONGLONG* Entries)
{
    const BC250_VIDMM* vm = &g_VidMm;
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
    *Physical = table + ((ULONGLONG)Update->StartIndex + Start) * sizeof(ULONGLONG);
    return TRUE;
}

typedef struct _BC250_PAGING_STREAM {
    BC250_DEVICE* Device;
    BC250_GFX* Gfx;
    ULONGLONG Root;
    BOOLEAN Fill;
    ULONG Pattern;
    ULONG CommandOffset;
    unsigned* Payload;
    BC250_WDDM_PAGING_UNSUPPORTED Unsupported;
} BC250_PAGING_STREAM;

static int PagingResolve(void* Context, PAGING_U64 Va, unsigned Bytes, PAGING_U64* Mc)
{
    BC250_PAGING_STREAM* stream = (BC250_PAGING_STREAM*)Context;
    ULONGLONG physical = 0;
    BOOLEAN system = FALSE;
    if (!VidMmTranslate(stream->Root, Va, &physical, &system)) {
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
        map.fill=stream->Fill; map.pattern=stream->Pattern;
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
        VidMmUpdatePageTable(Update);
        KeMemoryBarrier();
        *NextEntry = Update->NumPageTableEntries;
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


static void case_kmd_routes(void)
{
 static const struct amdgpu_vmhub_funcs funcs={NULL,test_invalidate_req};
 BC250_DEVICE dev={{0x200000000ll},0x100000000ull,0x100000,NULL,0};
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
 memset(&g_VidMm,0,sizeof(g_VidMm));g_VidMm.Ready=1;g_VidMm.Write=1;
 g_VidMm.SegmentPhysical=0x200000000ull;g_VidMm.SegmentLength=0x1000000;
 g_VidMm.Pte.units=BC250_PTE_ADDR_PAGES;g_VidMm.Pte.aperture=BC250_PTE_VM;
 g_VidMm.Pte.system_segment=0;g_VidMm.Pte.vram_segment=1;
 g_VidMm.Pte.vram_base=g_VidMm.SegmentPhysical;g_VidMm.Pte.vram_size=g_VidMm.SegmentLength;
 update.UpdateMode=DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;update.NumPageTableEntries=512;
 update.PageTableAddress.GpuPhysical.SegmentId=1;update.PageTableAddress.GpuPhysical.SegmentOffset=4096;
 update.pPageTableEntries=ptes;
 for(i=0;i<512;i++){ptes[i].Flags=BC250_DXGK_PTE_VALID | (1ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);ptes[i].PageAddress=100+i;}
 check(VidMmEncodePageTable(&update,0,512,&physical,encoded) && physical==0x200001000ull,"actual encoder translates full PTE source and table physical address");
 memset(buf,0xCC,sizeof(buf));cpu_pte_calls=0;
 rc=GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported);
 check(rc==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER && next==480 && written==974,"actual builder publishes480entries then requests next buffer");
 check(buf[1]==0x1000 && buf[2]==1 && buf[3]==959,"PTE table destination converted to MC");
 for(i=0;i<480;i++)if(buf[4+2*i]!=(u32)encoded[i] || buf[5+2*i]!=(u32)(encoded[i]>>32))break;
 check(i==480 && !cpu_pte_calls,"queued first batch preserves values without CPU table write");
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
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_SUCCESS && !written && next==512 && cpu_pte_calls==1,"pre-RUN bootstrap uses immediate CPU update");
 gfx.PagingCpuBootstrap=0;
 check(GfxPagingBuildUpdate(&dev,&update,buf,0,65536,0,&written,&next,&unsupported)==STATUS_DEVICE_NOT_READY && !written && !next && cpu_pte_calls==1,"after bootstrap close not-ready cannot fall back to CPU");
 update.StartIndex=1;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects table extent overflow");
 update.StartIndex=0;update.PageTableLevel=4;
 check(!VidMmEncodePageTable(&update,0,1,&physical,encoded),"encoder rejects invalid page-table level");
 check(!flush_lock_depth && !flush_region_depth,"PTE update exits balance lifetime locks");
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

	case_transfer_three_pages();
	case_fill_two_pages();
	case_insufficient();
	case_refusals();
	case_write_ptes();
	case_ordered_ptes();
	case_gart_invalidate();
	case_mapped_copy();
	case_kmd_routes();
	case_kmd_flush();
	case_kmd_updates();

	printf("\n== verdict ==\n");
	printf("  %u checks, %u failures\n", g_checks, g_failures);
	printf("  %s\n", g_failures == 0 ? "PASS" : "FAIL");
	return g_failures == 0 ? 0 : 1;
}
