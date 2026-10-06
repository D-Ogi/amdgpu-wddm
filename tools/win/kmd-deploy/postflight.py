"""LAB, read-only: independent postflight of an accepted and cleaned-up attempt.

    python postflight.py kmdRRR-deployNNN

Runs kmd-transition/postflight.ps1 of the attempt in a 30-second bounded child through ops/postflight-run.ps1. It
compares the live lab with the attempt's own baseline capture: candidate version/SYS/INF/ABI, confirmed health,
the complete graphics registration entry by entry and its files, hang detector values as captured, same boot, gates,
driver parameters, CPU desktop UMD/ICD, no competing process or task, watch task removed. The exit code is kept in
postflight-<n>.json next to the text, for accept.py. The confirmed-health check needs a fresh witness
(age_ms <= 15000), so the present heartbeat runs for the postflight (60 s, stopped afterwards).
"""
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import time
from kmdcommon import OPS, REMOTE_TMP, heartbeat, heartbeat_stop, next_receipt, resolve_attempt, target, verify_attempt  # noqa: E402


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    name, attempt, ops = resolve_attempt(sys.argv[1])
    manifest = verify_attempt(attempt, ops)
    t = target()
    print(heartbeat(t, 60))
    time.sleep(3)
    try:
        r = t.run_script(OPS / 'postflight-run.ps1', args=[name, manifest], timeout=60,
                         remote_dir=f'{REMOTE_TMP}\\{name}-ops')
    finally:
        print('heartbeat', heartbeat_stop(t))
    text = next_receipt(ops, 'postflight', '.txt')
    text.write_text(r.stdout + '\nSTDERR\n' + r.stderr, encoding='utf-8')
    text.with_suffix('.json').write_text(json.dumps({'utc': datetime.now(timezone.utc).isoformat(), 'returncode': r.returncode,
                                                     'manifest_sha256': manifest}) + '\n', encoding='utf-8')
    print(r.stdout[-4000:])
    print(r.stderr[-1000:])
    sys.exit(r.returncode)


if __name__ == '__main__':
    main()
