"""LAB: pull one attempt's whole remote directory (receipts, baseline, helper outputs) into its ops directory.

    python collect.py kmdRRR-deployNNN

Each call writes a new remote-evidence-<n>.tar and extracts its JSON/text receipts to collected-<n>/; nothing
earlier is overwritten, so run it again after Cleanup to keep the detector-final receipts too.
"""
import io
import json
import sys
import tarfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kmdcommon import REMOTE_BASE, next_receipt, resolve_attempt, target  # noqa: E402


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    name, attempt, ops = resolve_attempt(sys.argv[1])
    r = target().ssh(f'cmd /c "tar -cf - -C {REMOTE_BASE} {name}"', timeout=90, binary=True)
    if r.returncode or not r.stdout:
        raise SystemExit('archive pull failed')
    archive = next_receipt(ops, 'remote-evidence', '.tar')
    archive.write_bytes(r.stdout)
    dest = ops / archive.name.replace('remote-evidence', 'collected').replace('.tar', '')
    with tarfile.open(fileobj=io.BytesIO(r.stdout), mode='r') as tar:
        members = [m for m in tar.getmembers() if m.isfile() and m.name.rsplit('.', 1)[-1] in ('json', 'jsonl', 'txt', 'out', 'err')]
        tar.extractall(dest, members=members, filter='data')
    print('archive', archive.name, len(r.stdout), 'bytes; receipts in', dest.name)
    result = dest / name / 'watch-result.json'
    if result.exists():
        print(json.dumps(json.loads(result.read_text(encoding='utf-8-sig')), indent=1))
    else:
        print('no watch-result.json yet')


if __name__ == '__main__':
    main()
