/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The contract between the shim and whoever provides register access.
 *
 * Everything the imported amdgpu code does to the hardware funnels through exactly two functions.
 * `dword_index` is what amdgpu calls a register offset: the DWORD index that SOC15_REG_OFFSET
 * produces, so the byte offset inside BAR5 is dword_index * 4.
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

/* Read one 32-bit register. Must not have side effects beyond the hardware read itself. */
unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index);

/* Write one 32-bit register. */
void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value);

/* Busy-wait, amdgpu's udelay(). Kernel: KeStallExecutionProcessor. Host replay: nothing to wait
 * for, so a no-op. Called from bounded poll loops, never to sleep. */
void bc250_shim_udelay(unsigned int usec);

/* Logging behind dev_err/dev_info/dev_warn. Level: 0 info, 1 warning, 2 error. `dev` is whatever
 * the owner put in adev->dev; it is passed through so that the imports' `adev` argument stays
 * referenced (MSVC /W4 C4100) and a kernel backend can name the device. */
void bc250_shim_log(int level, void *dev, const char *fmt, ...);

#endif /* BC250_SHIM_H */
