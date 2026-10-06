import sys
from pathlib import Path

sys.path.insert(0, r"P:\bc-250\bc250-win\tools\win")
import target  # noqa: E402

here = Path(__file__).parent
t = target.Target()
r = t.run_script(str(here / "bench-b19.ps1"), timeout=200) if hasattr(t, "run_script") else None
print(r.stdout if hasattr(r, "stdout") else r)
if hasattr(r, "stderr") and r.stderr:
    print("STDERR:", r.stderr[-2000:])
