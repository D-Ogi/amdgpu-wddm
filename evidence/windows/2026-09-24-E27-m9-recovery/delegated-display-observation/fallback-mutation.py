from pathlib import Path
root=Path('P:/bc-250/bc250-win/driver/kmd');out=Path('P:/bc-250/scratch/m9/bd007-009')
s=(root/'wddm.c').read_text();needle='''        if (InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0) != generation) return;''';assert s.count(needle)==1
(out/'no-fallback-generation.c').write_text(s.replace(needle,'        /* negative control: accept changing fallback generation */'))
