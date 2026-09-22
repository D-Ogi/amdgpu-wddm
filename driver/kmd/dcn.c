// DCN 2.0.1 ("DMU") register dump (ADR 0011 point 3): a READ-ONLY escape that proves the offsets and the BAR5
// mapping are right under Windows before ADR 0011's first write. HUBPREQ0..3, HUBP0..3, OTG0..1 and
// DCHUBBUB_CTRL_STATUS - the registers a flip will read (and, later, write) before it ever moves the scanout.
//
// No gate of its own, no Start/Stop, no sequence, no Device state: MmioDcnRead answers as soon as BAR5 is mapped
// (EnableMmio), the same condition every other read-only escape here already needs, and refuses gracefully
// (STATUS_DEVICE_NOT_READY) when it is not. The registers themselves are on their own generated table
// (driver/kmd/gen_regs.py's DCN_REGISTERS, tools/regcalc, ip DMU) - never amdgpu's trace, because there is no
// Windows trace of this IP yet: proving these offsets from Windows is what this command is for.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"       // field masks for the decoded summary only; every offset comes from regcalc
#include <ntstrsafe.h>

void DcnEscape(_In_ const BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DCN* Data)
{
    const BC250_DCN_REG_INFO* table = NULL;
    ULONG count = MmioDcnTable(&table);
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG hubp0Lo = 0, hubp0Hi = 0, pitch = 0, otgControl = 0, syncStatus = 0;

    // Ties gen_regs.py's count (regs.generated.h) to the escape struct's fixed array (bc250kmd_escape.h): the two
    // are edited in different places and can only be kept equal by a check like every other one in this driver.
    C_ASSERT(BC250_DCN_REG_COUNT == BC250_DCN_REG_INFO_COUNT);

    Data->Version = BC250_KMD_VERSION;
    Data->RegCount = 0;
    Data->FaultOffset = 0;
    Data->Hubp0Address = 0;
    Data->Hubp0Pitch = 0;
    Data->Hubp0Cntl = 0;
    Data->Otg0Control = 0;
    Data->Otg0MasterEnable = 0;
    Data->Otg0HTotal = 0;
    Data->Otg0VTotal = 0;
    Data->Otg0VblankIntEnabled = 0;
    RtlZeroMemory(Data->Regs, sizeof(Data->Regs));

    if (Device->Mmio == NULL)
    {
        GuardLog("dcn: BAR5 not mapped (EnableMmio is closed)");
        Data->NtStatus = (unsigned long)STATUS_DEVICE_NOT_READY;
        Data->Status = BC250_ESCAPE_STATUS_REFUSED;
        return;
    }

    for (i = 0; i < count && i < BC250_DCN_REG_COUNT; i++)
    {
        ULONG value = 0;
        NTSTATUS regStatus = MmioDcnRead(Device, table[i].Offset, &value);

        if (!NT_SUCCESS(regStatus))
        {
            // Every offset in table[] came out of the same generated list MmioDcnRead checks against, so this is
            // not expected; if it ever happens it means the two disagreed, which is worth knowing rather than a
            // silently short dump.
            if (NT_SUCCESS(status)) { status = regStatus; Data->FaultOffset = table[i].Offset; }
            value = 0;
        }
        RtlStringCbCopyA(Data->Regs[i].Name, sizeof(Data->Regs[i].Name), table[i].Name);
        Data->Regs[i].Offset = table[i].Offset;
        Data->Regs[i].Value = value;
        Data->RegCount++;
    }

    // The decoded summary (evidence/linux/2026-09-22-E21-linux-reference-4/dmupre.txt is the Linux reference to
    // compare against): HUBP0 and OTG0 only, the pipe and timing generator the firmware is already scanning out
    // on. Read again by name rather than searched out of Regs[] above - eight more reads of registers the loop
    // just proved safe.
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, &hubp0Lo);
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, &hubp0Hi);
    Data->Hubp0Address = ((ULONGLONG)hubp0Hi << 32) | hubp0Lo;
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH, &pitch);
    Data->Hubp0Pitch = pitch & HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK;
    (void)MmioDcnRead(Device, BC250_REG_DMU_HUBP0_DCHUBP_CNTL, &Data->Hubp0Cntl);
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_CONTROL, &otgControl);
    Data->Otg0Control = otgControl;
    Data->Otg0MasterEnable = (otgControl & OTG0_OTG_CONTROL__OTG_MASTER_EN_MASK) ? 1 : 0;
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_H_TOTAL, &Data->Otg0HTotal);
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_V_TOTAL, &Data->Otg0VTotal);
    (void)MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS, &syncStatus);
    // Bit 12: AMD's own name for it in dcn_2_0_1_sh_mask.h is VUPDATE_NO_LOCK_INT_EN, not "vblank" - the field
    // name here is this escape's own, to compare against the Linux reference's timing, not a claim about AMD's.
    Data->Otg0VblankIntEnabled = (syncStatus & OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK) ? 1 : 0;

    GuardLog("dcn: %u registers, status 0x%08X; hubp0 addr 0x%llX pitch %u cntl 0x%08X; otg0 control 0x%08X h_total %u v_total %u",
             Data->RegCount, status, Data->Hubp0Address, Data->Hubp0Pitch, Data->Hubp0Cntl, Data->Otg0Control,
             Data->Otg0HTotal, Data->Otg0VTotal);
    Data->NtStatus = (unsigned long)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
