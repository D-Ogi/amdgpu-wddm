"""Start the present heartbeat and confirm the current KMD start (health flags 7 -> 15)."""
import sys
sys.path.insert(0, str(__import__('pathlib').Path(__file__).resolve().parents[1]))
import kmdcommon as k

t = k.target()
print(k.heartbeat(t, 180))
r = t.run_script(k.OPS / 'confirm-current.ps1', timeout=120)
print((r.stdout + r.stderr).strip())
