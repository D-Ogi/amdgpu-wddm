from pathlib import Path
root=Path('P:/bc-250/bc250-win/driver/kmd')
p=root/'gen_regs.py'; s=p.read_text()
s=s.replace('BAR5_LENGTH = 0x80000', '''# Internal observation only: keep the existing 75-register escape payload stable.
# AMD hubp1_is_flip_pending / optc1_get_crtc_scanoutpos (MIT).
DCN_PRIVATE_READ_REGISTERS = ["mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE",
    "mmHUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", "mmOTG0_OTG_V_BLANK_START_END"]
NAMED += [("DMU", name) for name in DCN_PRIVATE_READ_REGISTERS + ["mmOTG0_OTG_STATUS_POSITION"]]

BAR5_LENGTH = 0x80000''')
s=s.replace('    if dcn_allow[-1] >= BAR5_LENGTH:', '    dcn_allow = sorted(set(dcn_allow) | {offset(maps, "DMU", name) for name in DCN_PRIVATE_READ_REGISTERS})\n    if dcn_allow[-1] >= BAR5_LENGTH:')
s=s.replace('"// Sorted, unique, for MmioDcnRead\'s table check.",','"// Plus internal observation registers; sorted, unique, for MmioDcnRead\'s table check.",')
p.write_text(s)
p=root/'dcn.c'; s=p.read_text(); start=s.index('// The vsync DPC\'s own poll (WddmDcnVsync'); end=s.index('// The hardware vsync source\'s own enable/ack:',start)
s=s[:start]+'''// PROVENANCE: Linux AMD display dc/hubp/dcn10/dcn10_hubp.c (MIT),
// hubp1_is_flip_pending checks both the pending bit and EARLIEST_INUSE against
// the requested address. DCN addresses are CPU-physical here; dxgkrnl uses our
// MC-base-relative card addresses. Keep that conversion paired with the flip.
NTSTATUS DcnReadScanoutAddress(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* CardAddress)
{
    ULONG high, low, highAgain, attempt;
    ULONGLONG physical, offset;
    NTSTATUS status;

    *CardAddress = 0;
    if (Device->Mmio == NULL || !Device->VramEnabled) return STATUS_DEVICE_NOT_READY;
    // A latch between the two halves must not manufacture an address. Bounded
    // retries only: the caller can run at DPC and never waits for the next frame.
    for (attempt = 0; attempt < 3; ++attempt)
    {
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, &high);
        if (!NT_SUCCESS(status)) return status;
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, &low);
        if (!NT_SUCCESS(status)) return status;
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, &highAgain);
        if (!NT_SUCCESS(status)) return status;
        high &= HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
        highAgain &= HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
        if (high != highAgain) continue;
        physical = ((ULONGLONG)high << 32) | low;
        if (physical < (ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_DEVICE_NOT_READY;
        offset = physical - (ULONGLONG)Device->VramPhysical.QuadPart;
        if (offset >= Device->VramLength) return STATUS_DEVICE_NOT_READY;
        *CardAddress = Device->VramMcBase + offset;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_READY;
}

BOOLEAN DcnFlipPending(_In_ const BC250_DEVICE* Device, ULONGLONG RequestedAddress)
{
    ULONG value;
    ULONGLONG scanned;

    if (Device->Mmio == NULL) return TRUE;
    if (!NT_SUCCESS(MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &value))) return TRUE;
    if (value & HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return TRUE;
    if (!NT_SUCCESS(DcnReadScanoutAddress(Device, &scanned))) return TRUE;
    return scanned != RequestedAddress;
}

// PROVENANCE: Linux AMD display dc/optc/dcn10/dcn10_optc.c (MIT),
// optc1_get_position / optc1_get_crtc_scanoutpos. Sample the vertical counter,
// with the static mode's blank bounds, rather than a software timer's phase.
// Deriving blank from that same counter avoids a status/position boundary race.
NTSTATUS DcnReadScanLine(_In_ const BC250_DEVICE* Device, _Out_ BOOLEAN* InBlank, _Out_ UINT* ScanLine)
{
    ULONG bounds, total, position, start, end, line;
    NTSTATUS status;

    if (Device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_V_BLANK_START_END, &bounds);
    if (!NT_SUCCESS(status)) return status;
    status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_V_TOTAL, &total);
    if (!NT_SUCCESS(status)) return status;
    status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_STATUS_POSITION, &position);
    if (!NT_SUCCESS(status)) return status;
    start = (bounds & OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START_MASK) >> OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_START__SHIFT;
    end = (bounds & OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END_MASK) >> OTG0_OTG_V_BLANK_START_END__OTG_V_BLANK_END__SHIFT;
    total = ((total & OTG0_OTG_V_TOTAL__OTG_V_TOTAL_MASK) >> OTG0_OTG_V_TOTAL__OTG_V_TOTAL__SHIFT) + 1;
    line = (position & OTG0_OTG_STATUS_POSITION__OTG_VERT_COUNT_MASK) >> OTG0_OTG_STATUS_POSITION__OTG_VERT_COUNT__SHIFT;
    if (start >= total || end >= total || line >= total || start == end) return STATUS_DEVICE_NOT_READY;
    *InBlank = start < end ? (line >= start && line < end) : (line >= start || line < end);
    // D3D raster status starts line zero at the first active line (blank end).
    *ScanLine = *InBlank ? 0 : (line + total - end) % total;
    return STATUS_SUCCESS;
}

'''+s[end:]; p.write_text(s)
p=root/'wddm.c'; s=p.read_text(); s=s.replace('DcnFlipPending(Device))', 'DcnFlipPending(Device, (ULONGLONG)address))')
s=s.replace('''    // Refuse both an in-flight flip and a sample crossing a programming change.
''','''    // A stable completed request may be retired. Otherwise preserve this vblank
    // by reporting the buffer hardware is still reading, never the queued one.
''')
s=s.replace('''        InterlockedIncrement(&Device->DcnVsyncDeferred);
        return;
    }
    data.CrtcVsync.PhysicalAdapterMask''','''        ULONGLONG scanned;
        InterlockedIncrement(&Device->DcnVsyncDeferred); // completion deferred, not the vblank
        if (!NT_SUCCESS(DcnReadScanoutAddress(Device, &scanned))) return;
        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;
    }
    data.CrtcVsync.PhysicalAdapterMask''')
s=s.replace('''    // Answered from the timer's phase and nothing else: how far into the period we are gives the scan line, and
''','''    if (device->VidPnFlipEnabled)
    {
        NTSTATUS status = DcnReadScanLine(device, &pGetScanLine->InVerticalBlank, &pGetScanLine->ScanLine);
        if (!NT_SUCCESS(status)) return status;
        goto Report;
    }

    // Software-only path: how far into the period we are gives the scan line, and
''')
s=s.replace('''    if (WddmFirstCalls(wddm, WddmDdiGetScanLine))
''','''Report:
    if (WddmFirstCalls(wddm, WddmDdiGetScanLine))
'''); p.write_text(s)
p=root/'bc250kmd.h'; s=p.read_text(); old='''// The vsync DPC's own poll (wddm.c's WddmDcnVsync): a single, non-blocking read of
// HUBPREQ0_DCSURF_FLIP_CONTROL's SURFACE_FLIP_PENDING bit, never PollFlipPending's busy-wait. <= DISPATCH_LEVEL.
BOOLEAN DcnFlipPending(_In_ const BC250_DEVICE* Device);'''; new='''// Non-blocking hardware observations: pending includes EARLIEST_INUSE mismatch.
// Scanout returns the actual card address, independently of request publication.
BOOLEAN DcnFlipPending(_In_ const BC250_DEVICE* Device, ULONGLONG RequestedAddress);
NTSTATUS DcnReadScanoutAddress(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* CardAddress);
NTSTATUS DcnReadScanLine(_In_ const BC250_DEVICE* Device, _Out_ BOOLEAN* InBlank, _Out_ UINT* ScanLine);'''
assert old in s;s=s.replace(old,new);p.write_text(s)
