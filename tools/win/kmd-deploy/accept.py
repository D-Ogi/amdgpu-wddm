"""After an accepted promotion: move the deployed-KMD pins and print the STATE.md record.

    python accept.py kmdRRR-deployNNN            print the exact edits only
    python accept.py kmdRRR-deployNNN --apply    also write native-caps001/lab-baseline.json

Requires, from this attempt's own receipts: the newest collected watch-result `closed` with candidate_retained,
a Cleanup receipt, and the newest postflight exit 0 for the frozen manifest. The edits:
- lab-baseline.json: kmd_version, kmd_sys_sha256, kmd_abi and `since` (desktop UMD/ICD unchanged; the previous file
  is kept as lab-baseline-before.json in the ops directory). Native trial preflights read this file.
- identity pins: none to edit. Each freeze generates identity.ps1 from the packages and this file; the next
  promotion names this attempt's candidate directory as --rollback (printed below).
- STATE.md: printed, for the operator to place (its header rules how).
"""
import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from kmdcommon import LAB_BASELINE, TRANSITION, digest, identity, resolve_attempt, verify_attempt  # noqa: E402


def newest(ops, pattern):
    items = sorted(ops.glob(pattern), key=lambda p: int(re.search(r'-(\d+)', p.name).group(1)))
    return items[-1] if items else None


def main():
    args = [a for a in sys.argv[1:] if a != '--apply']
    if len(args) != 1:
        raise SystemExit(__doc__)
    name, attempt, ops = resolve_attempt(args[0])
    manifest_sha = verify_attempt(attempt, ops)
    manifest = json.loads((attempt / 'stage-manifest.json').read_text(encoding='utf-8'))
    ident = identity(attempt / TRANSITION / 'identity.ps1')
    if manifest['mode'] != ident['KmdDeployMode']:
        raise SystemExit('Only a deploy attempt can be accepted')
    collected = newest(ops, 'collected-*')
    result = collected / name / 'watch-result.json' if collected else None
    if not result or not result.exists():
        raise SystemExit('No collected watch-result.json: run collect.py')
    watch = json.loads(result.read_text(encoding='utf-8-sig'))
    if watch.get('status') != 'closed' or watch.get('candidate_retained') is not True:
        raise SystemExit(f'Not accepted: status {watch.get("status")}, candidate_retained {watch.get("candidate_retained")}')
    cleanup = newest(ops, 'Cleanup-*.txt')
    if not cleanup or not re.search(r'"state"\s*:\s*"Missing"', cleanup.read_text(encoding='utf-8', errors='replace')):
        raise SystemExit('No Cleanup receipt with the task Missing: run dispatch.py ... Cleanup')
    post = newest(ops, 'postflight-*.json')
    if not post:
        raise SystemExit('No postflight receipt: run postflight.py')
    post_r = json.loads(post.read_text(encoding='utf-8'))
    if post_r['returncode'] != 0 or post_r['manifest_sha256'] != manifest_sha:
        raise SystemExit(f'Postflight {post.name} did not pass for this manifest')
    hashes = json.loads((attempt / 'package-hashes.json').read_text(encoding='utf-8'))
    candidate = hashes[ident['KmdCandidateLabel']]
    before = json.loads(LAB_BASELINE.read_text(encoding='utf-8'))
    if (before['kmd_version'], before['kmd_sys_sha256'].upper()) != (ident['KmdRollbackVersion'], ident['KmdRollbackSysSha256']):
        raise SystemExit('lab-baseline.json no longer names this attempt\'s rollback; someone moved it, inspect')
    today = datetime.now(timezone.utc).strftime('%Y-%m-%d')
    after = dict(before)
    after.update({'since': f'{name} ({today}); desktop UMD/ICD unchanged since: {before["since"]}',
                  'kmd_version': ident['KmdCandidateVersion'], 'kmd_sys_sha256': candidate['bc250kmd.sys'],
                  'kmd_abi': ident['KmdCandidateAbi']})
    print('lab-baseline.json edits (' + str(LAB_BASELINE) + '):')
    for key in ('since', 'kmd_version', 'kmd_sys_sha256', 'kmd_abi'):
        print(f'  {key}: {before[key]!r} -> {after[key]!r}')
    print('\nSTATE.md (Current block, replaces the KMD line; move the old one to History):')
    print(f'- KMD {ident["KmdCandidateVersion"]} (SYS {candidate["bc250kmd.sys"][:8]}, INF {candidate["bc250kmd.inf"][:8]}, '
          f'CAT {candidate["bc250kmd.cat"][:8]}, ABI {ident["KmdCandidateAbi"]}; commit {manifest["candidate"]["commit"][:8]}, '
          f'package {manifest["candidate"]["source"]}), promoted by kmd-deploy {name} '
          f'(manifest {manifest_sha[:8]}, {watch.get("elapsed")} s) over {ident["KmdRollbackVersion"]} '
          f'(SYS {ident["KmdRollbackSysSha256"][:8]}, still in the DriverStore). Postflight {post.name} passed.')
    print('\nNext promotion rolls back to this one:')
    print(f'  python stage.py freeze --package <new package dir> --rollback {attempt / ident["KmdCandidateLabel"]}')
    if '--apply' in sys.argv:
        (ops / 'lab-baseline-before.json').write_text(json.dumps(before, indent=2) + '\n', encoding='utf-8')
        LAB_BASELINE.write_text(json.dumps(after, indent=2) + '\n', encoding='utf-8')
        print(f'\nwritten {LAB_BASELINE} ({digest(LAB_BASELINE)[:8]}); previous in {ops / "lab-baseline-before.json"}')


if __name__ == '__main__':
    main()
