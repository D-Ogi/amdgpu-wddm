/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A compute dispatch on a kernel-owned MEC queue. See include/bc250_dispatch.h for what this is
 * for, what it deliberately does differently from its reference, and why it is safe in VMID 0.
 *
 * Every packet below names the libdrm function and line it follows, in
 * P:\BC-250\ref\libdrm at tag libdrm-2.4.114, commit b9ca37b3134861048986b75896c0915cbf2e97f9,
 * tests/amdgpu/shader_test_util.c. The two packets that are ours and not libdrm's - the cache
 * invalidate in front and the flush and fence behind - name the amdgpu function instead, because
 * that is where an IB would have got them.
 *
 * No register offset, opcode or field shift is written out. The COMPUTE_* offsets are resolved
 * through AMD's own headers and then turned into packet offsets by subtracting the packet's base,
 * which is the arithmetic libdrm hardcodes:
 *
 *     SH packet offset = SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_x) - PACKET3_SET_SH_REG_START
 *
 * The one thing that IS a literal here is the shader binary, and it is not typed either: it comes
 * from third_party/libdrm/shader_code_gfx10.h, which is AMD's file byte for byte.
 */
#include "bc250_dispatch.h"
#include "bc250_gfx.h"          /* the fence emitter, BC250_ENOMEM and BC250_EIO */
#include "bc250_gmc.h"          /* BC250_EINVAL */
#include "bc250_shim.h"

#include "gc/gc_10_1_0_offset.h"
#include "soc15_common.h"
#include "nvd.h"
#include <navi10_enum.h>        /* VGT_EVENT_TYPE, for CS_PARTIAL_FLUSH */

/* libdrm's shader binaries, imported unmodified with their MIT notice
 * (third_party/libdrm/PROVENANCE.md). The file declares `static const` arrays, so like
 * clearstate_gfx10.h it is included in exactly one translation unit of the driver - this one - and
 * in the host test that checks what landed in memory.
 *
 * It also declares tables of `struct reg_info`, which libdrm defines in shader_code.h. That file
 * cannot come with it: it includes the whole shader-test world. The struct is two dwords and is
 * reproduced here from tests/amdgpu/shader_code.h:56-59 - the same thing the shim does for the
 * kernel headers, which is supply the handful of declarations an import reaches for rather than
 * edit the import. */
struct reg_info {
	uint32_t reg_offset;
	uint32_t reg_value;
};
#include "shader_code_gfx10.h"

/* ---------------------------------------------------------------------------------------------
 * The constants libdrm supplies, none of them derivable from a register header
 * ------------------------------------------------------------------------------------------- */

/*
 * Bit 1 of a type-3 header is the shader-type bit: it tells the CP that the SET_SH_REG writes that
 * follow name the COMPUTE SH register space and not the graphics one. libdrm sets it on every
 * packet of this sequence except the one uconfig write (shader_test_util.c:21-25,
 * PACKET3_COMPUTE). The kernel never emits a compute SET_SH_REG and so nvd.h has no macro for it;
 * this is that macro, over the imported PACKET3().
 */
#define BC250_PACKET3_COMPUTE(op, n)	(PACKET3(op, n) | (1u << 1))

/* shader_test_util.c:415-416. COMPUTE_PGM_LO is the shader address shifted right by 8, so the
 * buffer has to be 256-byte aligned; _HI is bits 47:40 of the byte address. The field widths agree:
 * COMPUTE_PGM_LO__DATA_MASK is 32 bits and COMPUTE_PGM_HI__DATA_MASK is 8, which is a 48-bit byte
 * address, and gmc_v10_0.c:864 says mc_mask is exactly 48 bits. */
#define BC250_DISPATCH_SHADER_ALIGN	256u

/* shader_test_util.c:436, the gfx10 value. A 128-bit buffer resource word 3: DST_SEL X,Y,Z,W,
 * FORMAT 0x4B (32_32_32_32 uint), RESOURCE_LEVEL 1 (required on gfx10.1), OOB_SELECT 1 (bounds
 * checked against NUM_RECORDS), TYPE 0 (buffer). Copied as a number because no header in either
 * tree names these fields; the decode is in the research note and costs nothing at runtime. */
#define BC250_DISPATCH_VDESC_W3		0x1104BFACu

/* shader_test_util.c:434. Bit 20 of the descriptor's second dword is STRIDE = 16 bytes, which is
 * what makes the record index in v4 address 16-byte records. */
#define BC250_DISPATCH_VDESC_STRIDE16	0x100000u

/* shader_code_gfx9.h:34-40, the five-entry register table the gfx10 shader reuses
 * (shader_code.h:102-104 hands the gfx9 table to the gfx10 shader on purpose). */
#define BC250_DISPATCH_PGM_RSRC1	0x000C0041u   /* 8 VGPRs, 16 SGPRs, CU mode, no scratch */
#define BC250_DISPATCH_PGM_RSRC2	0x00000090u   /* USER_SGPR = 8, TGID_X_EN = 1 */

/* shader_test_util.c:250, a uconfig write, and the only packet in the sequence WITHOUT the compute
 * bit. A performance knob for ACQUIRE_MEM; kept because the reference emits it. */
#define BC250_DISPATCH_COHER_START_DELAY 0x20u

/* shader_test_util.c:331-339. The offset dword of a SET_SH_REG_INDEX is index[31:28] | offset, and
 * index 3 routes the CU-mask registers through the CP's own path instead of writing them blind.
 * nvd.h defines the opcode (:576) and the kernel never emits it, so there is no in-tree statement
 * of what index 3 means; it is copied from the reference. */
#define BC250_DISPATCH_SH_REG_INDEX_CU	3u

/* shader_test_util.c:562. COMPUTE_SHADER_EN and nothing else. */
#define BC250_DISPATCH_INITIATOR	0x00000001u

/* ---------------------------------------------------------------------------------------------
 * Packet offsets, resolved rather than typed
 * ------------------------------------------------------------------------------------------- */

/*
 * A SET_SH_REG names a register by its offset from the packet's own base, and libdrm writes those
 * offsets out as numbers (0x204, 0x20c, 0x240 ...). Here they are the difference between what AMD's
 * header says the register is and what AMD's header says the base is, so nothing is typed and a
 * wrong one cannot survive a header update.
 *
 * Taking the resolved offset as an argument rather than the register name is deliberate:
 * SOC15_REG_OFFSET() pastes `reg` onto `_BASE_IDX`, and passing a register name through another
 * macro first would expand it to its number and paste `0x1ba4_BASE_IDX`, which is not a token.
 */
static u32 sh_off(u32 reg_dword)
{
	return reg_dword - PACKET3_SET_SH_REG_START;
}

static u32 uconfig_off(u32 reg_dword)
{
	return reg_dword - PACKET3_SET_UCONFIG_REG_START;
}

/* ---------------------------------------------------------------------------------------------
 * The two buffers
 * ------------------------------------------------------------------------------------------- */

static void seed_dst(struct amdgpu_device *adev)
{
	volatile u32 *p = (volatile u32 *)adev->gfx.dispatch_dst.cpu;
	unsigned int i;

	for (i = 0; i < BC250_DISPATCH_DST_BYTES / 4u; i++)
		p[i] = BC250_DISPATCH_SEED;
}

int bc250_gfx_dispatch_setup(struct amdgpu_device *adev)
{
	unsigned int i;
	volatile u32 *shader;
	int r;

	if (adev == NULL)
		return BC250_EINVAL;
	if (adev->gfx.dispatch_shader.cpu != NULL && adev->gfx.dispatch_dst.cpu != NULL)
		return 0;                       /* idempotent */

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, (u32)sizeof(bufferclear_cs_shader_gfx10),
				 BC250_DISPATCH_SHADER_ALIGN, &adev->gfx.dispatch_shader);
	if (r)
		return r;
	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_DISPATCH_DST_BYTES,
				 AMDGPU_GPU_PAGE_SIZE, &adev->gfx.dispatch_dst);
	if (r) {
		bc250_shim_mem_free(adev, &adev->gfx.dispatch_shader);
		return r;
	}
	if (adev->gfx.dispatch_shader.cpu == NULL || adev->gfx.dispatch_dst.cpu == NULL) {
		bc250_gfx_dispatch_teardown(adev);
		return BC250_ENOMEM;
	}

	/* The alignment is asked for, and then checked: the whole shader fetch depends on the low
	 * eight bits of this address being zero, and an allocator that ignored the request would
	 * otherwise show up as a dispatch that fetches nonsense. */
	if ((adev->gfx.dispatch_shader.mc & (BC250_DISPATCH_SHADER_ALIGN - 1u)) != 0) {
		dev_err(adev->dev, "dispatch shader at 0x%llX is not %u-byte aligned\n",
			(unsigned long long)adev->gfx.dispatch_shader.mc,
			BC250_DISPATCH_SHADER_ALIGN);
		bc250_gfx_dispatch_teardown(adev);
		return BC250_EINVAL;
	}

	shader = (volatile u32 *)adev->gfx.dispatch_shader.cpu;
	for (i = 0; i < ARRAY_SIZE(bufferclear_cs_shader_gfx10); i++)
		shader[i] = bufferclear_cs_shader_gfx10[i];

	seed_dst(adev);
	return 0;
}

void bc250_gfx_dispatch_teardown(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return;
	bc250_shim_mem_free(adev, &adev->gfx.dispatch_shader);
	bc250_shim_mem_free(adev, &adev->gfx.dispatch_dst);
}

int bc250_gfx_dispatch_reseed(struct amdgpu_device *adev)
{
	if (adev == NULL || adev->gfx.dispatch_dst.cpu == NULL)
		return BC250_EINVAL;
	seed_dst(adev);
	return 0;
}

u64 bc250_gfx_dispatch_shader_addr(const struct amdgpu_device *adev)
{
	return (adev == NULL) ? 0 : adev->gfx.dispatch_shader.mc;
}

u64 bc250_gfx_dispatch_dst_addr(const struct amdgpu_device *adev)
{
	return (adev == NULL) ? 0 : adev->gfx.dispatch_dst.mc;
}

/* ---------------------------------------------------------------------------------------------
 * The submission
 * ------------------------------------------------------------------------------------------- */

/* The 72 dwords of the dispatch itself, plus what an IB would have brought with it. */
#define BC250_DISPATCH_PACKET_DWORDS	72u
#define BC250_DISPATCH_ACQUIRE_DWORDS	8u
#define BC250_DISPATCH_FLUSH_DWORDS	2u

unsigned int bc250_gfx_dispatch_size(const struct amdgpu_ring *ring, unsigned int fence_flags)
{
	if (ring == NULL || ring->funcs == NULL ||
	    ring->funcs->type != AMDGPU_RING_TYPE_COMPUTE)
		return 0;
	return BC250_DISPATCH_ACQUIRE_DWORDS + BC250_DISPATCH_PACKET_DWORDS +
	       BC250_DISPATCH_FLUSH_DWORDS + bc250_gfx_fence_size(ring, fence_flags);
}

/* gfx_v10_0.c:9473-9494 gfx_v10_0_emit_mem_sync(), which amdgpu_ib_schedule() puts in front of
 * every IB (amdgpu_ib.c:80 sets the flag, :211-212 acts on it). Submitting to the ring directly
 * means emitting it here, and it is not optional: without it the shader can be fetched through a
 * stale GL1/GL2 line. */
static void emit_mem_sync(struct amdgpu_ring *ring)
{
	const unsigned int gcr_cntl =
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GL2_WB(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLM_WB(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GL1_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLV_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLK_INV(1) |
			PACKET3_ACQUIRE_MEM_GCR_CNTL_GLI_INV(1);

	amdgpu_ring_write(ring, PACKET3(PACKET3_ACQUIRE_MEM, 6));
	amdgpu_ring_write(ring, 0);             /* CP_COHER_CNTL */
	amdgpu_ring_write(ring, 0xffffffff);    /* CP_COHER_SIZE */
	amdgpu_ring_write(ring, 0xffffff);      /* CP_COHER_SIZE_HI */
	amdgpu_ring_write(ring, 0);             /* CP_COHER_BASE */
	amdgpu_ring_write(ring, 0);             /* CP_COHER_BASE_HI */
	amdgpu_ring_write(ring, 0x0000000A);    /* POLL_INTERVAL */
	amdgpu_ring_write(ring, gcr_cntl);
}

/* One SET_SH_REG naming `count` consecutive registers from `first`. Every SET_SH_REG in the
 * sequence has this shape, so it is one helper rather than eighteen open-coded headers. */
static void set_sh(struct amdgpu_ring *ring, u32 packet_offset, const u32 *values, u32 count)
{
	u32 i;

	amdgpu_ring_write(ring, BC250_PACKET3_COMPUTE(PACKET3_SET_SH_REG, count));
	amdgpu_ring_write(ring, packet_offset);
	for (i = 0; i < count; i++)
		amdgpu_ring_write(ring, values[i]);
}

static void set_sh1(struct amdgpu_ring *ring, u32 packet_offset, u32 value)
{
	set_sh(ring, packet_offset, &value, 1u);
}

int bc250_gfx_dispatch_memset(struct amdgpu_ring *ring, u32 value, u32 groups,
			      u64 fence_addr, u64 seq, unsigned int flags)
{
	struct amdgpu_device *adev;
	u64 shader_addr, dst_addr;
	u32 zero[6];
	u32 v[4];
	unsigned int ndw;
	int r;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL)
		return BC250_EINVAL;
	if (ring->funcs->type != AMDGPU_RING_TYPE_COMPUTE)
		return BC250_EINVAL;
	if (groups == 0 || groups > BC250_DISPATCH_MAX_GROUPS)
		return BC250_EINVAL;

	adev = ring->adev;
	if (adev->gfx.dispatch_shader.cpu == NULL || adev->gfx.dispatch_dst.cpu == NULL)
		return BC250_EINVAL;

	shader_addr = adev->gfx.dispatch_shader.mc;
	dst_addr = adev->gfx.dispatch_dst.mc;

	ndw = bc250_gfx_dispatch_size(ring, flags);
	r = amdgpu_ring_alloc(ring, ndw);
	if (r)
		return r;

	/* Ours, not libdrm's: what amdgpu_ib_schedule() would have put in front. */
	emit_mem_sync(ring);

	memset(zero, 0, sizeof(zero));

	/* amdgpu_dispatch_init_gfx10(), shader_test_util.c:230-253, which begins by running
	 * amdgpu_dispatch_init_gfx9() (:206-228) and then adds the gfx10-only writes. */
	set_sh(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_START_X)), zero, 3u);        /* :218-220 */
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_TMPRING_SIZE)), 0);         /* :223-225, no scratch */
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_SHADER_CHKSUM)), 0);        /* :240-242 */
	set_sh(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_REQ_CTRL)), zero, 6u);       /* :244-246 */

	/* :248-250. A uconfig register, and the one packet here with no compute bit. */
	amdgpu_ring_write(ring, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	amdgpu_ring_write(ring, uconfig_off(SOC15_REG_OFFSET(GC, 0, mmCP_COHER_START_DELAY)));
	amdgpu_ring_write(ring, BC250_DISPATCH_COHER_START_DELAY);

	/* amdgpu_dispatch_write_cumask(), shader_test_util.c:331-339: all CUs of all four SEs, through
	 * the CP's CU-mask path. The MQD already sets these four fields to the same value
	 * (gfx_v10_0.c:6914-6917), so this is the reference being followed rather than a requirement;
	 * if the CP ever rejects the opcode, these two packets are the ones to drop. */
	amdgpu_ring_write(ring, BC250_PACKET3_COMPUTE(PACKET3_SET_SH_REG_INDEX, 2));
	amdgpu_ring_write(ring, (BC250_DISPATCH_SH_REG_INDEX_CU << 28) |
				sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_STATIC_THREAD_MGMT_SE0)));
	amdgpu_ring_write(ring, 0xffffffff);
	amdgpu_ring_write(ring, 0xffffffff);
	amdgpu_ring_write(ring, BC250_PACKET3_COMPUTE(PACKET3_SET_SH_REG_INDEX, 2));
	amdgpu_ring_write(ring, (BC250_DISPATCH_SH_REG_INDEX_CU << 28) |
				sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_STATIC_THREAD_MGMT_SE2)));
	amdgpu_ring_write(ring, 0xffffffff);
	amdgpu_ring_write(ring, 0xffffffff);

	/* amdgpu_dispatch_write2hw_gfx10(), shader_test_util.c:403-463. */
	v[0] = (u32)(shader_addr >> 8);                                 /* :415 COMPUTE_PGM_LO */
	v[1] = (u32)(shader_addr >> 40);                                /* :416 COMPUTE_PGM_HI */
	set_sh(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_LO)), v, 2u);

	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC1)), BC250_DISPATCH_PGM_RSRC1);
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC2)), BC250_DISPATCH_PGM_RSRC2);
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_X)), BC250_DISPATCH_THREADS_PER_GROUP);
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_Y)), 1u);
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_NUM_THREAD_Z)), 1u);
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_PGM_RSRC3)), 0);            /* :426-428 */

	/* :431-436, the buffer resource the shader stores through, in USER_DATA_0..3. The address is
	 * raw and unshifted here, unlike the program address above.
	 *
	 * NUM_RECORDS is what THIS dispatch covers, not the size of the allocation: libdrm derives
	 * both it and DIM_X from the same `dst.size` (:434 and :560), so the descriptor bounds the
	 * stores to exactly the records the workgroups are going to write. Measured that way on unit
	 * A under Linux - fact M49, the g1 run has NUM_RECORDS = 0x40 with DIM_X = 1 and the g16 run
	 * 0x400 with DIM_X = 16 (evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-
	 * windows/dispatch/g1.txt:56, g16.txt) - so keeping it means our submission is dword for
	 * dword the one that is known to work on this silicon. */
	v[0] = (u32)dst_addr;
	v[1] = (u32)(dst_addr >> 32) | BC250_DISPATCH_VDESC_STRIDE16;
	v[2] = groups * BC250_DISPATCH_THREADS_PER_GROUP;               /* NUM_RECORDS */
	v[3] = BC250_DISPATCH_VDESC_W3;
	set_sh(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_0)), v, 4u);

	/* :439-444, the fill pattern in USER_DATA_4..7, which the shader moves into v0..v3. libdrm
	 * writes 0x22222222 four times; the caller chooses the dword here so a second run can be told
	 * from the first. */
	v[0] = value;
	v[1] = value;
	v[2] = value;
	v[3] = value;
	set_sh(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_USER_DATA_4)), v, 4u);

	/* amdgpu_dispatch_write_dispatch_cmd(), shader_test_util.c:547-565. */
	set_sh1(ring, sh_off(SOC15_REG_OFFSET(GC, 0, mmCOMPUTE_RESOURCE_LIMITS)), 0);      /* :553-555 */

	amdgpu_ring_write(ring, BC250_PACKET3_COMPUTE(PACKET3_DISPATCH_DIRECT, 3));
	amdgpu_ring_write(ring, groups);                                /* DIM_X */
	amdgpu_ring_write(ring, 1);                                     /* DIM_Y */
	amdgpu_ring_write(ring, 1);                                     /* DIM_Z */
	amdgpu_ring_write(ring, BC250_DISPATCH_INITIATOR);

	/* Ours again. The partial flush makes the CP wait for the waves to retire before the fence
	 * behind it is allowed to signal; without it the fence can land while the shader is still
	 * writing. gfx_v10_0_ring_emit_pipeline_sync() is the gfx-ring equivalent; on compute the
	 * event is what upstream uses (nvd.h:343 names the index). */
	amdgpu_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE, 0));
	amdgpu_ring_write(ring, EVENT_TYPE(CS_PARTIAL_FLUSH) |
				EVENT_INDEX(PACKET3_EVENT_WRITE__EVENT_INDEX__CS_PARTIAL_FLUSH));

	/* The fence closes the submission and writes GL2 back on its way out: its RELEASE_MEM carries
	 * GCR_GL2_WB and the CACHE_FLUSH_AND_INV_TS event (bc250_gfx_emit_fence()), which is what makes
	 * the destination readable by the CPU afterwards. */
	r = bc250_gfx_emit_fence(ring, fence_addr, seq, flags);
	if (r) {
		amdgpu_ring_undo(ring);
		return r;
	}

	amdgpu_ring_commit(ring);
	return 0;
}

int bc250_gfx_dispatch_check(struct amdgpu_device *adev, u32 value, u32 groups,
			     u32 *first_bad_offset)
{
	const volatile u32 *p;
	u32 written_dwords, i;

	if (first_bad_offset != NULL)
		*first_bad_offset = 0;
	if (adev == NULL || adev->gfx.dispatch_dst.cpu == NULL)
		return BC250_EINVAL;
	if (groups == 0 || groups > BC250_DISPATCH_MAX_GROUPS)
		return BC250_EINVAL;

	p = (const volatile u32 *)adev->gfx.dispatch_dst.cpu;
	written_dwords = groups * (BC250_DISPATCH_GROUP_BYTES / 4u);

	for (i = 0; i < BC250_DISPATCH_DST_BYTES / 4u; i++) {
		u32 want = (i < written_dwords) ? value : BC250_DISPATCH_SEED;

		if (p[i] == want)
			continue;
		if (first_bad_offset != NULL)
			*first_bad_offset = i * 4u;
		return BC250_EIO;
	}
	return 0;
}
