/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_IH_FAULT_H
#define BC250_IH_FAULT_H
// KMD193: the UTCL2 interrupt vector of a GPU page fault, decoded. Written for the 147/208/209/245 class of
// 0x116 bugchecks (scratch game-recon bsod-245, REPORT-245.md): the fault is in the IH ring of the dump, but
// the driver never said a word about it while it was running, because ih.c's Note() only counts client/source
// pairs and keeps the last sixteen vectors. One line per second of faults turns that into a live witness.
//
// No WDK header here on purpose: the decode and the per-second bucket are the same code in the DPC and in the
// host test (driver/kmd/test/ih_fault_test.c, quality gate ih-fault). Field masks come from the vendored AMD
// header, offsets from tools/regcalc through regs.generated.h - nothing is typed by hand.
#include "gc_10_1_0_sh_mask.h"      // GCVM_L2_PROTECTION_FAULT_STATUS field masks; offsets come from regcalc

// soc15_ih_clientid.h: SOC15_IH_CLIENTID_UTCL2. The gfxhub's faults arrive on it (amdgpu
// gmc_v10_0_process_interrupt treats anything but SOC15_IH_CLIENTID_VMC as the gfxhub).
#define BC250_IH_UTCL2_CLIENT 0x1Bu
// amdgpu gmc_v10_0_process_interrupt (reference/linux v6.18 gmc_v10_0.c:106-113): the fault address is
// src_data[0] << 12 with bits 44-47 from src_data[1], bit 0x20 says write, bit 0x80 says retry.
#define BC250_IH_FAULT_WRITE 0x20u
#define BC250_IH_FAULT_RETRY 0x80u
#define BC250_IH_FAULT_VA_HI_MASK 0xFu

static __inline unsigned long long Bc250IhFaultVa(unsigned long SrcData0, unsigned long SrcData1)
{
    return ((unsigned long long)SrcData0 << 12) |
           (((unsigned long long)SrcData1 & BC250_IH_FAULT_VA_HI_MASK) << 44);
}

static __inline int Bc250IhFaultIsWrite(unsigned long SrcData1)
{
    return (SrcData1 & BC250_IH_FAULT_WRITE) != 0;
}

static __inline int Bc250IhFaultIsRetry(unsigned long SrcData1)
{
    return (SrcData1 & BC250_IH_FAULT_RETRY) != 0;
}

// ---- the latched GCVM_L2_PROTECTION_FAULT_STATUS -------------------------------------------------------------
// Fields of gc_10_1_0_sh_mask.h. The hardware latches the status and the address of the FIRST fault of a burst
// and sets MORE_FAULTS for the rest, which is why the DPC reads the three registers once per burst and not once
// per vector.
static __inline unsigned long Bc250GcvmFaultField(unsigned long Status, unsigned long Mask, unsigned long Shift)
{
    return (Status & Mask) >> Shift;
}
#define BC250_GCVM_FAULT_CID(status) \
    Bc250GcvmFaultField((status), GCVM_L2_PROTECTION_FAULT_STATUS__CID_MASK, \
                        GCVM_L2_PROTECTION_FAULT_STATUS__CID__SHIFT)
#define BC250_GCVM_FAULT_VMID(status) \
    Bc250GcvmFaultField((status), GCVM_L2_PROTECTION_FAULT_STATUS__VMID_MASK, \
                        GCVM_L2_PROTECTION_FAULT_STATUS__VMID__SHIFT)
#define BC250_GCVM_FAULT_PERMISSIONS(status) \
    Bc250GcvmFaultField((status), GCVM_L2_PROTECTION_FAULT_STATUS__PERMISSION_FAULTS_MASK, \
                        GCVM_L2_PROTECTION_FAULT_STATUS__PERMISSION_FAULTS__SHIFT)
#define BC250_GCVM_FAULT_WALKER_ERROR(status) \
    Bc250GcvmFaultField((status), GCVM_L2_PROTECTION_FAULT_STATUS__WALKER_ERROR_MASK, \
                        GCVM_L2_PROTECTION_FAULT_STATUS__WALKER_ERROR__SHIFT)
#define BC250_GCVM_FAULT_MORE(status) \
    Bc250GcvmFaultField((status), GCVM_L2_PROTECTION_FAULT_STATUS__MORE_FAULTS_MASK, \
                        GCVM_L2_PROTECTION_FAULT_STATUS__MORE_FAULTS__SHIFT)
#define BC250_GCVM_FAULT_MAPPING(status) \
    Bc250GcvmFaultField((status), GCVM_L2_PROTECTION_FAULT_STATUS__MAPPING_ERROR_MASK, \
                        GCVM_L2_PROTECTION_FAULT_STATUS__MAPPING_ERROR__SHIFT)
#define BC250_GCVM_FAULT_RW(status) \
    Bc250GcvmFaultField((status), GCVM_L2_PROTECTION_FAULT_STATUS__RW_MASK, \
                        GCVM_L2_PROTECTION_FAULT_STATUS__RW__SHIFT)

// The names of the UTCL2 clients the gfxhub reports in CID, in the gfxhub's own order. Taken from
// driver/amdgpu-import/gfxhub_v2_0.c `gfxhub_client_ids` (Linux v6.18, MIT, see that directory's
// PROVENANCE.md), which is static there and so cannot be linked against. An unknown index is named, never
// indexed: the table is the hardware's, and a wider CID field on a later part must not read past it.
#define BC250_GFXHUB_CLIENT_COUNT 18u
static __inline const char* Bc250GfxhubClientName(unsigned long Cid)
{
    static const char* const names[BC250_GFXHUB_CLIENT_COUNT] = {
        "CB/DB", "Reserved", "GE1", "GE2", "CPF", "CPC", "CPG", "RLC", "TCP", "SQC (inst)",
        "SQC (data)", "SQG", "Reserved", "SDMA0", "SDMA1", "GCR", "SDMA2", "SDMA3"
    };
    return Cid < BC250_GFXHUB_CLIENT_COUNT ? names[Cid] : "unknown";
}

// ---- the per-second bucket -----------------------------------------------------------------------------------
// 245 took 225 fault vectors in 114 microseconds. One log line per vector would be 225 DbgPrintEx calls inside
// one DPC; one line per second of faults is a witness that costs nothing when nothing is wrong and still names
// the first page, VMID and direction of every burst. Pure: the caller supplies the second, the register reads
// and the logging happen outside the IH stats lock (ih.c Consume).
typedef struct BC250_IH_FAULT_STATE {
    unsigned long long Total;           // UTCL2 vectors seen since the ring was enabled
    unsigned long long Second;          // the whole second (interrupt time) the open bucket counts
    unsigned long long FirstVa;         // the GPU VA of the bucket's first vector
    unsigned long InSecond;             // vectors counted in the open bucket
    unsigned long PreviousInSecond;     // what the bucket before it counted (0 for the first one)
    unsigned long Bursts;               // buckets opened; also the number of report lines owed
    unsigned long VmId, RingId, SourceId, SrcData1;      // the bucket's first vector
    int Write, Retry;
} BC250_IH_FAULT_STATE;

// 1 when this vector opens a new bucket, i.e. the caller owes one report line and one read of the latch.
static __inline int Bc250IhFaultNote(BC250_IH_FAULT_STATE* Fault, unsigned long long Second, unsigned long VmId,
                                     unsigned long RingId, unsigned long SourceId, unsigned long SrcData0,
                                     unsigned long SrcData1)
{
    Fault->Total++;
    if (Fault->Bursts != 0 && Fault->Second == Second) { Fault->InSecond++; return 0; }
    Fault->PreviousInSecond = Fault->Bursts != 0 ? Fault->InSecond : 0;
    Fault->Second = Second;
    Fault->InSecond = 1;
    Fault->Bursts++;
    Fault->FirstVa = Bc250IhFaultVa(SrcData0, SrcData1);
    Fault->VmId = VmId;
    Fault->RingId = RingId;
    Fault->SourceId = SourceId;
    Fault->SrcData1 = SrcData1;
    Fault->Write = Bc250IhFaultIsWrite(SrcData1);
    Fault->Retry = Bc250IhFaultIsRetry(SrcData1);
    return 1;
}

#endif
