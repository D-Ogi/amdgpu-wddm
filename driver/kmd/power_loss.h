/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* BD-188 D-2: a retained way down that fails while the bus really takes the power (docs/design/power-loss.md).
 * Pure decisions, no hardware access, so driver/kmd/test/power_loss_test.c checks them on the host.
 *
 * 1. Bc250PowerLossExpected: may a failed step of the way down go on in the forced form (power.c)? Only when the
 *    power IRP says that the adapter loses its state: a D3 target, or a known system sleep or hibernate.
 * 2. The witness: one GPU register that this driver writes with a pattern as the last act of such a way down, and
 *    reads at D0 before any restore. Which register is per-IP data: Bc250PowerWitnessFor maps the GC version of
 *    the device (asic_ip.h) to a witness id, and the shim resolves the id to an offset (SOC15_REG_OFFSET). An IP
 *    without a witness answers BC250_POWER_WITNESS_NONE, so its verdict is always "no proof".
 * 3. Bc250PowerWitnessVerdict: lost (0), kept (the pattern) or no proof (anything else, or no read). Only "lost"
 *    allows the restore after an unsaved way down; the other two end in FAILED, as before D-2.
 * 4. Bc250HangPowerLost: the ResetEngine rule for a job that was on a ring when the power went. */
#ifndef BC250_POWER_LOSS_H
#define BC250_POWER_LOSS_H

#include "asic_ip.h"

/* The values of the WDK's DEVICE_POWER_STATE and POWER_ACTION that the rule reads (wdm.h). power.c checks them
 * against the WDK enums at compile time. */
#define BC250_POWER_DEVICE_D3        4u  /* PowerDeviceD3 */
#define BC250_POWER_ACTION_SLEEP     2u  /* PowerActionSleep: S1-S3 */
#define BC250_POWER_ACTION_HIBERNATE 3u  /* PowerActionHibernate: S4 */

/* 1 when a failed step of the way down to Target may go on in the forced form. dxgkrnl sends D3 only when it
 * gives the adapter to the bus, and the driver cannot tell D3hot from D3cold at that time: the witness at D0
 * decides what really happened. D1 or D2 without a sleep action keeps the rule of FAILED at the first failed step. */
static __inline int Bc250PowerLossExpected(unsigned target, unsigned action)
{
    return target == BC250_POWER_DEVICE_D3 || action == BC250_POWER_ACTION_SLEEP ||
           action == BC250_POWER_ACTION_HIBERNATE;
}

/* The witness registers. */
#define BC250_POWER_WITNESS_NONE            0u
#define BC250_POWER_WITNESS_GC_SCRATCH_REG1 1u  /* GC mmSCRATCH_REG1: no driver step and no ring test writes it
                                                 * (the producer check of docs/design/power-loss.md) */

/* The pattern: bits in both halves, not 0 (the power-on value) and not all ones (a read that the bus refused). */
#define BC250_POWER_WITNESS_PATTERN 0x5AC3A53Cu

/* The witness of one GC version. GC 10.1 has SCRATCH_REG1 in the register headers that the shim compiles
 * (third_party/linux-amdgpu/gc_10_1_0_offset.h, 0x2041). Another GC version gets a row here only together with
 * its own header and its own producer check. */
static __inline unsigned Bc250PowerWitnessFor(unsigned gc)
{
    return BC250_IP_MAJOR(gc) == 10u && BC250_IP_MINOR(gc) == 1u ? BC250_POWER_WITNESS_GC_SCRATCH_REG1
                                                                  : BC250_POWER_WITNESS_NONE;
}

/* The witness of a device by its PCI id (configuration dword 0): NONE for a device that is not in asic_ip.h. */
static __inline unsigned Bc250PowerWitnessOfDevice(unsigned pciId)
{
    BC250_ASIC_IP ip;

    if (!Bc250AsicIpLookup(BC250_PCI_VENDOR(pciId), BC250_PCI_DEVICE(pciId), &ip)) return BC250_POWER_WITNESS_NONE;
    return Bc250PowerWitnessFor(ip.Gc);
}

#define BC250_POWER_WITNESS_UNPROVEN 0u  /* no armed witness, a refused read, or a value that proves nothing */
#define BC250_POWER_WITNESS_LOST     1u  /* the register reads its power-on value 0 */
#define BC250_POWER_WITNESS_KEPT     2u  /* the register still holds the pattern */

/* armed: the way down wrote the pattern and read it back. readOk: the D0 read returned a value. */
static __inline unsigned Bc250PowerWitnessVerdict(int armed, int readOk, unsigned value)
{
    if (!armed || !readOk) return BC250_POWER_WITNESS_UNPROVEN;
    if (value == 0u) return BC250_POWER_WITNESS_LOST;
    if (value == BC250_POWER_WITNESS_PATTERN) return BC250_POWER_WITNESS_KEPT;
    return BC250_POWER_WITNESS_UNPROVEN;
}

/* ResetEngine: the head job of the node was on its ring when the power went (lostSeq, the newest ring sequence of
 * the node at the forced pause, 0 for none), so no wave or queue of the restored engine holds it. The wave kill and
 * the queue reset cannot recover it; the adapter reset can (docs/design/power-loss.md section 6). Sequences wrap,
 * so the comparison is a signed difference. */
static __inline int Bc250HangPowerLost(int onRing, unsigned headSeq, unsigned lostSeq)
{
    return onRing && lostSeq != 0u && (int)(headSeq - lostSeq) <= 0;
}

#endif
