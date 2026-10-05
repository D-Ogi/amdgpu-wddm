from pathlib import Path
import shutil
r=Path('bc250-win/driver/kmd');out=Path('scratch/m9/bd013-014')
for name,old,new in [('observe-unsync','EscapeFlags!=1u','EscapeFlags!=0u'),('observe-wrong-register','BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE,','BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS,'),('observe-stale-sequence','Data->SequenceAfter=(ULONG)InterlockedCompareExchange(&Device->DcnSurfaceSequence,0,0);','Data->SequenceAfter=Data->SequenceBefore;')]:
    d=out/name;d.mkdir(exist_ok=True)
    for f in ['dcn.c','display.c','display_timing_snapshot.h']:shutil.copy2(r/f,d/f)
    p=d/'dcn.c';s=p.read_text();assert old in s;s=s.replace(old,new,1);p.write_text(s)
