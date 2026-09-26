from pathlib import Path
r=Path('P:/bc-250/bc250-win/driver/kmd')
p=r/'wddm.c';s=p.read_text();old='''    // 2026-09-22 (ADR 0011 consequences): the present path's own destination mapping (dcn.c), torn down here -
    // first in the stop order (docs/design/vidpn-flip.md section 8) - so it is gone before DcnStop's own
    // restore-to-firmware write runs and before MmioStop unmaps BAR5, whatever happens to either of those.
    DcnUnmapScanout(Device);
''';assert old in s;s=s.replace(old,'    // DcnStop already released the CPU scanout alias before the verified restore.\n');p.write_text(s)
p=r/'test/post_display_stop_test.c';s=p.read_text();s=s.replace('static void DcnUnmapScanout(BC250_DEVICE*d){(void)d;CHECK(model.restores==1);}\n','');p.write_text(s)
p=r/'pnp.c';s=p.read_text();needle='''    NTSTATUS status;
    RtlZeroMemory(DisplayInfo,sizeof(*DisplayInfo));''';assert s.count(needle)==1;s=s.replace(needle,'''    NTSTATUS status;
    UNREFERENCED_PARAMETER(TargetId); // one active output; return its actual id below
    RtlZeroMemory(DisplayInfo,sizeof(*DisplayInfo));''');s=s.replace('DisplayInfo->TargetId = TargetId==D3DDDI_ID_UNINITIALIZED ? BC250_CHILD_UID : TargetId;', 'DisplayInfo->TargetId = BC250_CHILD_UID;');p.write_text(s)
p=r/'test/post_display_stop_test.c';s=p.read_text().replace('#define TRUE 1','#define UNREFERENCED_PARAMETER(x) ((void)(x))\n#define TRUE 1');p.write_text(s)
# Freeze mutation inputs by copying only the actual-source extraction inputs.
out=Path('P:/bc-250/scratch/m9/bd003-011')
for mutant in ['no-bugcheck-restore','no-inuse-check','false-handover-success','late-restore']:
    d=out/mutant;d.mkdir(exist_ok=True)
    for f in ['dcn.c','display.c','pnp.c','wddm.c']:(d/f).write_bytes((r/f).read_bytes())
p=out/'no-bugcheck-restore/display.c';s=p.read_text().replace('status=DcnRestorePostDisplay(device);','status=STATUS_SUCCESS; /* negative: no scanout restoration */');p.write_text(s)
p=out/'no-inuse-check/dcn.c';s=p.read_text().replace('return scanned==Device->DcnFirmwareAddress && pitch==Device->DcnFirmwarePitch ?', '(void)scanned; return pitch==Device->DcnFirmwarePitch ?');p.write_text(s)
p=out/'false-handover-success/pnp.c';s=p.read_text().replace('    if (!NT_SUCCESS(device->PostDisplayStopStatus)) return device->PostDisplayStopStatus;', '    /* negative: handover falsely claims success */');p.write_text(s)
p=out/'late-restore/wddm.c';s=p.read_text();needle='''    Device->PostDisplayStopStatus=DcnStop(Device);
    Device->PostDisplayStopAttempted=TRUE;
''';assert s.count(needle)==1;s=s.replace(needle,'');s=s.replace('    VidMmStop();\n','    VidMmStop();\n'+needle);p.write_text(s)
