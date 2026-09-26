from pathlib import Path
r=Path('P:/bc-250/bc250-win/driver/kmd')
p=r/'dcn.c';s=p.read_text();signature='static NTSTATUS DcnReadScanoutPhysical(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Physical)';start=s.index(signature+'\n{');end=s.index('\n}\n',start)+3;body=s[start:end];s=s[:start]+s[end:];assert s.count(signature+';')==1;s=s.replace(signature+';',body);p.write_text(s)
p=r/'bc250kmd.h';s=p.read_text().replace('    NTSTATUS PostDisplayStopStatus;', '    BOOLEAN PostDisplayStopAttempted;  // WddmStop restores before freeing scanout objects\n    NTSTATUS PostDisplayStopStatus;');p.write_text(s)
p=r/'wddm.c';s=p.read_text();needle='''    KeFlushQueuedDpcs();                // join readers admitted before detach, even while IH remains enabled
    while (wddm->PagingHead)''';replacement='''    KeFlushQueuedDpcs();                // join readers admitted before detach, even while IH remains enabled
    // OS level-three exclusion has stopped flip DDIs and our DPCs are joined.
    // Restore the reserved POST surface before VidMm/object release can retire
    // the buffer DCN was scanning. Keep failure distinct from StopDevice success.
    Device->PostDisplayStopStatus=DcnStop(Device);
    Device->PostDisplayStopAttempted=TRUE;
    while (wddm->PagingHead)''';assert s.count(needle)==1;s=s.replace(needle,replacement);p.write_text(s)
p=r/'pnp.c';s=p.read_text();needle='''    SmuOwnerStop(&device->Smu); // join clients before any engine/translation teardown
''';replace=needle+'''    device->SystemDisplayReady=FALSE;
    device->PostDisplayStopAttempted=FALSE;
    device->PostDisplayStopStatus=STATUS_DEVICE_NOT_READY;
''';assert s.count(needle)==1;s=s.replace(needle,replace)
needle='''    WddmStop(device);       // first: it logs what dxgkrnl called, and nothing below it is allowed to have run
''';replacement=needle+'''    // Display-only starts have no WDDM object and therefore no WddmStop restore.
    if (!device->PostDisplayStopAttempted) {
        device->PostDisplayStopStatus=DcnStop(device);
        device->PostDisplayStopAttempted=TRUE;
    }
''';assert s.count(needle)==1;s=s.replace(needle,replacement)
old='''    // ADR 0011, consequences: if a flip ever moved HUBP0 off the firmware's own address, put it back before
    // BAR5 goes away. A no-op, logged as one, when nothing ever flipped (dcn.c's DcnStop).
    DcnStop(device);
''';assert old in s;s=s.replace(old,'')
start=s.index('    // The mode is the one we were given;',s.index('NTSTATUS Bc250StopDeviceAndReleasePostDisplayOwnership('));end=s.index('\n}\n',start)
s=s[:start]+'''    NTSTATUS status;
    RtlZeroMemory(DisplayInfo,sizeof(*DisplayInfo));
    // Local dispmprt contract: failure makes dxgkrnl call ordinary StopDevice.
    // Never publish a framebuffer that DCN failed to latch. Ordinary StopDevice
    // still finishes teardown and returns its own completion result.
    status=Bc250StopDevice(MiniportDeviceContext);
    if (!NT_SUCCESS(status)) return status;
    if (!NT_SUCCESS(device->PostDisplayStopStatus)) return device->PostDisplayStopStatus;
    *DisplayInfo = device->Post;
    DisplayInfo->TargetId = TargetId==D3DDDI_ID_UNINITIALIZED ? BC250_CHILD_UID : TargetId;
    return STATUS_SUCCESS;'''+s[end:];p.write_text(s)
