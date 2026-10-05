"""LAB: drive the supervisor of one frozen attempt on unit A.

    python dispatch.py kmdRRR-deployNNN Prepare|Start|Inspect|Cleanup|RestoreDetector

The manifest hash comes from attempts/<name>-ops/stage-manifest.sha256 after the local tree is re-verified against
it. Every answer is kept as <Mode>-<n>.txt in the ops directory. Start first starts the present heartbeat for
200 s (the 180 s task and its confirmations need a fresh start-health witness); if it does not start, nothing does.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kmdcommon import REMOTE_BASE, TRANSITION, heartbeat, next_receipt, resolve_attempt, target, verify_attempt  # noqa: E402

MODES = ('Prepare', 'Start', 'Inspect', 'Cleanup', 'RestoreDetector')


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in MODES:
        raise SystemExit(__doc__)
    name, attempt, ops = resolve_attempt(sys.argv[1])
    mode = sys.argv[2]
    manifest = verify_attempt(attempt, ops)
    remote = REMOTE_BASE + '\\' + name
    cmd = (f'powershell -NoProfile -ExecutionPolicy Bypass -File {remote}\\{TRANSITION}\\dispatch.ps1 -Mode {mode}'
           f' -Directory {remote} -ManifestSha256 {manifest}')
    t = target()
    if mode == 'Start':
        print(heartbeat(t, 200))
    r = t.ssh(cmd, timeout=40)
    next_receipt(ops, mode, '.txt').write_text(r.stdout + '\nSTDERR\n' + r.stderr, encoding='utf-8')
    print(r.stdout[-5000:])
    print(r.stderr[-1000:])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
