/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The contract between the shim and whoever provides register access.
 *
 * Everything the imported amdgpu code does to a register funnels through exactly two functions.
 * `dword_index` is what amdgpu calls a register offset: the DWORD index that SOC15_REG_OFFSET
 * produces, so the byte offset inside BAR5 is dword_index * 4.
 *
 * M5 adds the two things the CP needs besides registers: a doorbell write and an allocator for
 * GPU-visible memory. The shim still owns no memory and maps nothing - the caller does both.
 *
 * Backends:
 *   driver/shim/test/backend_trace.c   host replay: reads answer from unit A's pre-driver sweep,
 *                                      writes are recorded and compared with the amdgpu trace.
 *   driver/kmd (later)                 the miniport: MmioRead/MmioWrite in driver/kmd/mmio.c,
 *                                      allow-list checked, byte offset = dword_index * 4.
 */
#ifndef BC250_SHIM_H
#define BC250_SHIM_H

struct amdgpu_device;

/* Where a buffer lives. VRAM is the local frame buffer, addressed by its MC address; GTT is system
 * memory made visible to the GPU through the GART that M4 brought up. Both appear on unit A's E03
 * trace: the MQDs and the clear-state buffer are VRAM, the rings and the writeback slots are GTT. */
enum bc250_mem_domain {
	BC250_MEM_VRAM,
	BC250_MEM_GTT
};

/* One allocation. `cpu` may be NULL if the owner cannot map the memory; the shim then refuses to
 * build anything that needs CPU access rather than guessing. `mc` is what the GPU uses, i.e.
 * amdgpu_bo_gpu_offset(). */
struct bc250_mem {
	void *cpu;
	unsigned long long mc;
	unsigned int size;
};

/* Read one 32-bit register. Must not have side effects beyond the hardware read itself. */
unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index);

/* Write one 32-bit register. */
void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value);

/* Busy-wait, amdgpu's udelay(). Kernel: KeStallExecutionProcessor. Host replay: nothing to wait
 * for, so a no-op. Called from bounded poll loops, never to sleep. */
void bc250_shim_udelay(unsigned int usec);

/* Write 64 bits into the doorbell aperture. `index` is the doorbell index amdgpu uses, which is a
 * dword index into that aperture, so the byte offset is index * 4 and the store spans two dwords
 * (upstream amdgpu_mm_wdoorbell64 does exactly this: adev->doorbell.cpu_addr is a uint32_t *).
 * Not optional on GFX10: gfx_v10_0_ring_set_wptr_compute() calls BUG() when a compute or KIQ ring
 * has no doorbell, so there is no MMIO fallback to fall back to.
 *   Kernel: a 64-bit write to the mapped doorbell BAR.
 *   Host replay: recorded, like a register write but in its own log. */
void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index,
			    unsigned long long value);

/* Write 32 bits into the doorbell aperture, at the same dword index. amdgpu's WDOORBELL32
 * (amdgpu_mm_wdoorbell, amdgpu_device.c) as against WDOORBELL64.
 *
 * The width is not cosmetic and it is not a choice. Upstream uses the 32-bit store in exactly one
 * place on this part - navi10_ih_set_rptr()'s doorbell arm (navi10_ih.c:474 and :499) - and the
 * 64-bit store everywhere else (gfx_v10_0.c:8571 and :8605 for the gfx and compute write pointers,
 * sdma_v5_0.c:389 for SDMA). A 64-bit store at the IH's index would also write the next dword with
 * zero, which is a doorbell amdgpu never rings; the IH doorbell window is two entries wide
 * (BIF_IH_DOORBELL_RANGE SIZE = 2), so that dword is inside the window and would reach the hardware.
 *
 *   Kernel: one 32-bit write to the mapped doorbell BAR (driver/kmd/gpumem.c).
 *   Host replay: recorded with its width, so a test can insist on it.
 *
 * DPC note: this is the one shim call bc250_ih_set_rptr() makes at DISPATCH_LEVEL, so the kernel
 * implementation must not log, allocate or take a lock. See bc250_ih.h. */
void bc250_shim_wdoorbell32(struct amdgpu_device *adev, unsigned int index, unsigned int value);

/* Allocate GPU-visible memory. The shim never allocates: the miniport hands out VRAM by physical
 * address and GART-mapped system pages, the host test hands out plain memory plus the MC addresses
 * unit A used. Must zero the allocation (amdgpu's buffer objects are zeroed on create, and the MQD
 * builders rely on it). `align` is in bytes. Returns 0 or a negative error code. */
int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain,
			 unsigned int size, unsigned int align, struct bc250_mem *out);

/* Release an allocation. Must tolerate a zeroed struct (nothing was ever allocated into it), so that
 * an error path can free everything it has without tracking what it got. Must also release an
 * allocation whose `cpu` is NULL (the owner could not map it, see the comment on `struct bc250_mem`
 * above): `mc` and `size` still name it, and a backend that looks it up by `cpu` alone can never give
 * it back - see driver/shim/test/sdma_faults_report.md, D-02. */
void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *m);

/* Logging behind dev_err/dev_info/dev_warn. Level: 0 info, 1 warning, 2 error. `dev` is whatever
 * the owner put in adev->dev; it is passed through so that the imports' `adev` argument stays
 * referenced (MSVC /W4 C4100) and a kernel backend can name the device. */
void bc250_shim_log(int level, void *dev, const char *fmt, ...);

#endif /* BC250_SHIM_H */
