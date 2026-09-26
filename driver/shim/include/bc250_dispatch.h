/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * A compute dispatch on a kernel-owned MEC queue: the smallest thing that proves the shader
 * pipeline runs, and the last piece of milestone M6.
 *
 * What it does: a 64-thread memset shader writes a 16-byte record per thread into GART memory, one
 * workgroup per 64 records, and the CPU reads the result back. A fence on the same submission says
 * when it is finished. Every part of that - the shader binary, the PM4 sequence, the two magic
 * descriptor dwords - is transcribed from libdrm's own gfx10 dispatch test rather than invented:
 *
 *   <BC250_ROOT>\ref\libdrm, tag libdrm-2.4.114, commit b9ca37b3134861048986b75896c0915cbf2e97f9
 *   tests/amdgpu/shader_test_util.c   amdgpu_test_dispatch_memset() at :566 and the four
 *                                     functions it calls, :206-253, :309-344, :403-463, :547-565
 *   tests/amdgpu/shader_code_gfx10.h  bufferclear_cs_shader_gfx10, :27-31
 *
 * The shader words are not retyped: third_party/libdrm/shader_code_gfx10.h is that file byte for
 * byte, with its MIT notice, verified against the checkout (third_party/libdrm/PROVENANCE.md).
 *
 * Three things this does NOT follow libdrm in, each with its reason:
 *
 *  1. No indirect buffer. libdrm builds the 72 dwords into a GTT buffer and submits an IB
 *     (shader_test_util.c:590-594, :630-640). Nothing in the sequence is IB-only, so the packets go
 *     straight into the compute ring. That removes the IB address, the IB pool and the pad-to-256
 *     rule (amdgpu_ring.c:160-164) from the list of things that can be wrong on a first attempt.
 *  2. GART, not VRAM. libdrm puts both buffers in VRAM (shader_test_util.c:598, :605). Both are in
 *     GART-mapped system memory here, which is where gfx_v10_0_ring_test_ib() puts its own target
 *     (gfx_v10_0.c:4083-4089) and what the CPU can read without a VRAM window.
 *  3. The cache handling is explicit. An IB gets an ACQUIRE_MEM in front of it from
 *     amdgpu_ib_schedule (amdgpu_ib.c:80, :211-212, gfx_v10_0_emit_mem_sync() at
 *     gfx_v10_0.c:9473-9494) and a cache flush behind it from the fence. Submitting to the ring
 *     directly means emitting both: this file does, so the shader cannot fetch stale instruction
 *     bytes and the CPU cannot read a result still sitting in GL2.
 *
 * The same 72 dwords and the same nine-dword shader were run on unit A under Linux before any of
 * this was written, through raw ioctls on AMDGPU_HW_IP_COMPUTE: one workgroup filled 256 of 256
 * dwords, sixteen filled 4096 of 4096, with and without a kernel-prepended ACQUIRE_MEM (fact M49,
 * evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/dispatch/). So the shader
 * is good for gfx1013, the program is good for gfx1013, and both shapes of cache handling work.
 * What that run does NOT say anything about is the two departures it could not exercise: it went
 * through an INDIRECT_BUFFER with VMID 2 and a user page table, not straight into a kernel ring at
 * VMID 0 with a GART address. Those two are still ours to be right about, and they are why the
 * paragraph below spells out why VMID 0 is fetchable.
 *
 * VMID 0 throughout, which is what a KIQ-mapped kernel compute queue already is - gfx10_kiq_map_queues()
 * hardcodes VMID(0) (gfx_v10_0.c:3751), the MQD forces it (:6961-6963, :7001) - and VMID 0 is the
 * flat GART aperture, page table depth 0 (gfxhub_v2_0.c:254-264). Instruction fetch goes through the
 * same path as data and the GART PTEs carry AMDGPU_PTE_EXECUTABLE (gmc_v10_0.c:761-762), so a shader
 * in GART is fetchable. No scratch: COMPUTE_PGM_RSRC2.SCRATCH_EN is 0 and COMPUTE_TMPRING_SIZE is 0,
 * which is what lets this work although gfx_v10_0_constants_init() deliberately leaves SH_MEM_BASES
 * alone for VMID 0 (gfx_v10_0.c:5359).
 */
#ifndef BC250_DISPATCH_H
#define BC250_DISPATCH_H

#include "amdgpu.h"

/* The destination buffer, and the shape the shader writes it in.
 *
 * 16 bytes per thread (a BUFFER_STORE_FORMAT_XYZW of four dwords), 64 threads per workgroup
 * (COMPUTE_NUM_THREAD_X, shader_code_gfx9.h:37-39), so one workgroup covers 1 KB. The buffer is
 * libdrm's own 0x4000 bytes (shader_test_util.c:604), which is 1024 records or 16 workgroups. */
#define BC250_DISPATCH_RECORD_BYTES	16u
#define BC250_DISPATCH_THREADS_PER_GROUP 64u
#define BC250_DISPATCH_GROUP_BYTES	(BC250_DISPATCH_RECORD_BYTES * BC250_DISPATCH_THREADS_PER_GROUP)
#define BC250_DISPATCH_DST_BYTES	0x4000u
#define BC250_DISPATCH_MAX_GROUPS	(BC250_DISPATCH_DST_BYTES / BC250_DISPATCH_GROUP_BYTES)

/* What the destination is filled with before the dispatch, so that "the shader did not run" and
 * "the shader wrote the wrong thing" are different answers. It is the value the kernel's own ring
 * and IB tests seed with (gfx_v10_0.c:4045, :4088). */
#define BC250_DISPATCH_SEED		0xCAFEDEADu

/* Allocate the shader buffer and the destination, copy the shader in, seed the destination.
 *
 * The shader buffer is 256-byte aligned because COMPUTE_PGM_LO takes the address shifted right by 8
 * (shader_test_util.c:415-416); the low eight bits are not in the register and an unaligned shader
 * would be fetched from the wrong place. Idempotent. Returns 0 or BC250_EINVAL.
 *
 * Allocated on its own and never from bc250_gfx_setup(), for the reason adev->gfx.fence_mem carries:
 * an allocation the traced bring-up does not make must not move the addresses it programs. */
int  bc250_gfx_dispatch_setup(struct amdgpu_device *adev);
void bc250_gfx_dispatch_teardown(struct amdgpu_device *adev);

/* Fill the destination with the seed again, without reallocating. Call before a second dispatch, or
 * the check cannot tell a fresh write from the previous one. */
int  bc250_gfx_dispatch_reseed(struct amdgpu_device *adev);

/* How many dwords bc250_gfx_dispatch_memset() writes, so the caller can size a reservation or
 * compare against the ring's free space. The fence flags are the ones that will be passed to it. */
unsigned int bc250_gfx_dispatch_size(const struct amdgpu_ring *ring, unsigned int fence_flags);

/*
 * Emit the whole submission into `ring` and commit it: cache invalidate, the dispatch, a compute
 * partial flush, then a fence at `fence_addr`/`seq` with `flags` (AMDGPU_FENCE_FLAG_INT for an
 * interrupt, nothing for the quiet control). The doorbell is rung on return; the caller waits by
 * polling the fence slot, or on the interrupt.
 *
 * `value` is the dword the shader stores, repeated four times per record - libdrm uses 0x22222222
 * (shader_test_util.c:441-444). `groups` is the number of 64-thread workgroups, 1 to
 * BC250_DISPATCH_MAX_GROUPS; 1 writes the first kilobyte and is the right first attempt on
 * hardware, 16 writes the whole buffer and is libdrm's own dispatch. It also sets the buffer
 * descriptor's NUM_RECORDS, as libdrm does, so the hardware itself bounds the stores to the range
 * the workgroups are meant to cover and a wrong DIM_X cannot reach past it.
 *
 * Returns 0, or BC250_EINVAL with nothing written (no setup, wrong ring type, groups out of range,
 * a fence address the fence emitter refuses), or what amdgpu_ring_alloc() returned.
 *
 * Compute queues only. The gfx ring would need CONTEXT_CONTROL and the graphics SH register space;
 * upstream's compute ring ops carry no .emit_cntxcntl at all (gfx_v10_0.c:9887-9925) and libdrm's
 * write_context_control() is a no-op for compute (shader_test_util.c:135-139), which is the same
 * statement from the other side.
 */
int  bc250_gfx_dispatch_memset(struct amdgpu_ring *ring, u32 value, u32 groups,
			       u64 fence_addr, u64 seq, unsigned int flags);

/*
 * Read the destination back and say whether the dispatch did exactly what it was asked.
 *
 * Every dword of the first `groups` workgroups must be `value`, and every dword after them must
 * still be BC250_DISPATCH_SEED - a dispatch that wrote past its workgroup count is as wrong as one
 * that wrote nothing, and only checking the far end catches it. `first_bad_offset`, if given, gets
 * the byte offset of the first dword that fails, or 0 when everything passes.
 *
 * Returns 0, BC250_EINVAL (no setup or groups out of range), or BC250_EIO for a mismatch.
 */
int  bc250_gfx_dispatch_check(struct amdgpu_device *adev, u32 value, u32 groups,
			      u32 *first_bad_offset);

/* The two MC addresses, for logging and for a hardware run that wants to dump them. 0 if the
 * buffers are not allocated. */
u64  bc250_gfx_dispatch_shader_addr(const struct amdgpu_device *adev);
u64  bc250_gfx_dispatch_dst_addr(const struct amdgpu_device *adev);

#endif /* BC250_DISPATCH_H */
