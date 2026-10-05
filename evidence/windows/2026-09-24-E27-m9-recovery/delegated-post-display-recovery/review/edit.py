from pathlib import Path
r=Path('P:/bc-250/bc250-win/driver/kmd')
p=r/'dcn.c';s=p.read_text();start=s.index('NTSTATUS DcnReadScanoutAddress(');end=s.index('\nBOOLEAN DcnFlipPending(',start)
old=s[start:end]
physical=old.replace('NTSTATUS DcnReadScanoutAddress(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* CardAddress)', 'static NTSTATUS DcnReadScanoutPhysical(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Physical)')
physical=physical.replace('ULONGLONG physical, offset;', 'ULONGLONG physical;').replace('*CardAddress = 0;', '*Physical = 0;').replace('Device->Mmio == NULL || !Device->VramEnabled', 'Device->Mmio == NULL')
a=physical.index('        if (physical < ');b=physical.index('        return STATUS_SUCCESS;',a)
physical=physical[:a]+'''        *Physical = physical;
'''+physical[b:]
wrapper='''NTSTATUS DcnReadScanoutAddress(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* CardAddress)
{
    ULONGLONG physical, offset;
    NTSTATUS status;
    *CardAddress = 0;
    if (!Device->VramEnabled) return STATUS_DEVICE_NOT_READY;
    status = DcnReadScanoutPhysical(Device, &physical);
    if (!NT_SUCCESS(status)) return status;
    if (physical < (ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_DEVICE_NOT_READY;
    offset = physical - (ULONGLONG)Device->VramPhysical.QuadPart;
    if (offset >= Device->VramLength) return STATUS_DEVICE_NOT_READY;
    *CardAddress = Device->VramMcBase + offset;
    return STATUS_SUCCESS;
}
'''
s=s[:start]+physical+'\n'+wrapper+s[end:]
start=s.index("// The stop path's undo");end=s.index('// ---- ADR 0011 point 3 step 3',start)
s=s[:start]+'''// Restore is shared by orderly stop and the bugcheck display. Only cached
// nonpaged state, checked MMIO, interlocked counters and bounded processor stalls
// are used. In particular, no pool, scheduler wait, log, software lock or GPU
// reset is reachable here. OS bugcheck/stop serialization owns the display.
static NTSTATUS DcnReadScanoutPhysical(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Physical);
static NTSTATUS DcnCheckPostSurface(_In_ const BC250_DEVICE* Device)
{
    ULONG pending, pitch;
    ULONGLONG scanned;
    NTSTATUS status;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,&pending);
    if (!NT_SUCCESS(status)) return status;
    if (pending&HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return STATUS_DEVICE_BUSY;
    status=DcnReadScanoutPhysical(Device,&scanned);
    if (!NT_SUCCESS(status)) return status;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&pitch);
    if (!NT_SUCCESS(status)) return status;
    pitch=((pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)+1)*4;
    return scanned==Device->DcnFirmwareAddress && pitch==Device->DcnFirmwarePitch ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
}

NTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device)
{
    NTSTATUS status;
    ULONG waited=0;
    // Captured before any address write, so this also covers a bugcheck midway
    // through a flip before DcnDiverged/current-address publication completed.
    if (!Device->DcnFirmwareKnown)
        return Device->DcnDiverged ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
    if (!Device->Framebuffer || Device->DcnFirmwarePitch!=Device->Post.Pitch)
        return STATUS_DEVICE_NOT_READY;
    status=DcnCheckPostSurface(Device);
    if (!NT_SUCCESS(status))
    {
        status=DcnFlipWriteSequence(Device,Device->DcnFirmwareAddress,Device->DcnFirmwarePitch,TRUE);
        if (!NT_SUCCESS(status)) return status;
        for (;;)
        {
            status=DcnCheckPostSurface(Device);
            if (status!=STATUS_DEVICE_BUSY) break;
            if (waited>=BC250_DCNFLIP_POLL_MAX_US) return STATUS_IO_TIMEOUT;
            KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
            waited+=BC250_DCNFLIP_POLL_STEP_US;
        }
        if (!NT_SUCCESS(status)) return status;
    }
    Device->DcnCurrentAddress=Device->DcnFirmwareAddress;
    Device->DcnCurrentPitch=Device->DcnFirmwarePitch;
    Device->DcnDiverged=FALSE;
    return STATUS_SUCCESS;
}

NTSTATUS DcnStop(_Inout_ BC250_DEVICE* Device)
{
    NTSTATUS status;
    DcnUnmapScanout(Device);
    status=DcnRestorePostDisplay(Device);
    GuardLog("dcnflip: stop: verified firmware scanout status 0x%08X",status);
    return status;
}

'''+s[end:];p.write_text(s)
p=r/'bc250kmd.h';s=p.read_text();s=s.replace('    SIZE_T FramebufferLength;', '    SIZE_T FramebufferLength;\n    BOOLEAN SystemDisplayReady;        // bugcheck CPU writes only after verified scanout restore\n    NTSTATUS PostDisplayStopStatus;    // separate from the ordinary StopDevice completion result');s=s.replace('void DcnStop(_Inout_ BC250_DEVICE* Device);','NTSTATUS DcnStop(_Inout_ BC250_DEVICE* Device);\nNTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device); // any IRQL; no allocation/log/lock');p.write_text(s)
p=r/'display.c';s=p.read_text();s=s.replace('''    Device->Framebuffer = NULL;
    Device->FramebufferLength = 0;''','''    Device->SystemDisplayReady = FALSE;
    Device->Framebuffer = NULL;
    Device->FramebufferLength = 0;''')
start=s.index('    UNREFERENCED_PARAMETER(TargetId);',s.index('NTSTATUS Bc250SystemDisplayEnable('));end=s.index('\n}\n',start)
s=s[:start]+'''    NTSTATUS status;
    UNREFERENCED_PARAMETER(Flags);
    device->SystemDisplayReady=FALSE;
    *Width=0;*Height=0;*ColorFormat=D3DDDIFMT_UNKNOWN;
    // Single inherited output, advertised as always connected by QueryChildStatus.
    if (TargetId!=BC250_CHILD_UID && TargetId!=D3DDDI_ID_UNINITIALIZED) return STATUS_NOT_SUPPORTED;
    if (!device->Framebuffer || !IsPostFormatSupported(device->Post.ColorFormat) ||
        !device->Post.Width || !device->Post.Height ||
        (ULONGLONG)device->Post.Width*4>device->Post.Pitch ||
        (ULONGLONG)device->Post.Pitch*device->Post.Height>device->FramebufferLength)
        return STATUS_DEVICE_NOT_READY;
    status=DcnRestorePostDisplay(device);
    if (!NT_SUCCESS(status)) return status;
    *Width = device->Post.Width;
    *Height = device->Post.Height;
    *ColorFormat = device->Post.ColorFormat;
    device->SystemDisplayReady=TRUE;
    return STATUS_SUCCESS;'''+s[end:]
s=s.replace('''    if (device->Framebuffer == NULL || PositionX >= device->Post.Width || PositionY >= device->Post.Height) return;
    width = min(SourceWidth, device->Post.Width - PositionX);''','''    if (!device->SystemDisplayReady || device->Framebuffer == NULL ||
        PositionX >= device->Post.Width || PositionY >= device->Post.Height) return;
    width = min(SourceWidth, device->Post.Width - PositionX);
    width = min(width, SourceStride / 4);''');p.write_text(s)
p=r/'test/generate_dcn_observation_test.py';s=p.read_text().replace("    'NTSTATUS DcnReadScanoutAddress(',", "    'static NTSTATUS DcnReadScanoutPhysical(', 'NTSTATUS DcnReadScanoutAddress(',");p.write_text(s)
