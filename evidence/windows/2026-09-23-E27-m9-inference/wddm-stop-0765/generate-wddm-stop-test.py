"""Extract actual WddmStop cancellation/detach/flush order; inject a late DPC."""
from pathlib import Path
import sys
here=Path(__file__).resolve().parent
s=Path(sys.argv[1]).read_text();a=s.index('void WddmStop(')
a=s.index('    KeAcquireSpinLock(&wddm->Lock, &irql);',a)
b=s.index('    VidMmStop();',a)
code=(here/'wddm-stop-test-prefix.c').read_text()+'static void Stop(DEVICE* Device) { WDDM* wddm=Device->Wddm; int irql;\n'+s[a:b]+'}\n'+(here/'wddm-stop-test-suffix.c').read_text()
Path(sys.argv[2]).write_text(code)
