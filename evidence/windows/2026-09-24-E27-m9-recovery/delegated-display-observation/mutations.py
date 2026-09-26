from pathlib import Path
root=Path('P:/bc-250/bc250-win/driver/kmd');out=Path('P:/bc-250/scratch/m9/bd007-009')
s=(root/'dcn.c').read_text();assert s.count('return scanned != RequestedAddress;')==1
(out/'no-earliest.c').write_text(s.replace('return scanned != RequestedAddress;','return FALSE; /* negative control */'))
s=(root/'wddm.c').read_text();needle='''    if (device->VidPnFlipEnabled)
    {
        NTSTATUS status = DcnReadScanLine''';assert needle in s
(out/'timer-scanline.c').write_text(s.replace(needle,'''    if (FALSE)
    {
        NTSTATUS status = DcnReadScanLine'''))
needle='''        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;''';assert s.count(needle)==1
(out/'drop-deferred.c').write_text(s.replace(needle,'return; /* negative control: lose the valid old-buffer vblank */'))
needle='''    if (InterlockedCompareExchange(&Wddm->PrimarySequence, 0, 0) != generation) return FALSE;''';assert s.count(needle)==1
(out/'no-generation.c').write_text(s.replace(needle,'    /* negative control: accept a crossing generation */'))
p=root/'bc250kmd.h';s=p.read_text();s=s.replace('''// found at least one VUPDATE_NO_LOCK event acknowledged (Device->DcnVsyncAcked), deferred one tick if the flip
// is still pending. A pending flip reports the distinct scanned buffer when the
// generation is stable.''','''// found at least one VUPDATE_NO_LOCK event acknowledged (Device->DcnVsyncAcked).
// A pending flip reports the distinct scanned buffer when the generation is stable;
// ambiguous observations remain deferred rather than retiring an unlatched flip.''');p.write_text(s)
