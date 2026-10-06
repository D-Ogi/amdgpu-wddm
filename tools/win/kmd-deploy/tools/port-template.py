"""One-shot: turn the copied kmd174-transition template into the version-free kmd-transition template.
Kept as the derivation record; it refuses to run twice."""
from pathlib import Path

K = Path(__file__).resolve().parent.parent
OLD = K / 'template/kmd174-transition'
T = K / 'template/kmd-transition'
if not OLD.exists():
    raise SystemExit('already ported')


def sub(path, pairs):
    raw = path.read_bytes()
    for old, new in pairs:
        o, n = old.encode('utf-8'), new.encode('utf-8')
        if o not in raw:
            raise SystemExit(f'{path.name}: missing {old!r}')
        raw = raw.replace(o, n)
    path.write_bytes(raw)


OLD.rename(T)
for old, new in (('preflight173.ps1', 'preflight.ps1'), ('postflight174.ps1', 'postflight.ps1'),
                 ('confirmed-present-start174.ps1', 'confirmed-present-start.ps1')):
    (T / old).rename(T / new)
names = [('kmd174-transition', 'kmd-transition'), ('preflight173', 'preflight'), ('postflight174', 'postflight'),
         ('confirmed-present-start174', 'confirmed-present-start')]
for path in list(T.glob('*.ps1')) + list((K / 'ops').glob('*.ps1')):
    raw = path.read_bytes()
    for old, new in names:
        raw = raw.replace(old.encode(), new.encode())
    path.write_bytes(raw)

# identity.ps1 in the template is a FIXTURE for the host tests (174 over 173); stage.py freeze writes the real one
# of each attempt from the two package manifests and native-caps001/lab-baseline.json.
sub(T / 'identity.ps1', [
    ("# The only place that names this promotion: 0.7.174.1 (CU mode 24/40) candidate over the deployed 0.7.173.1,\n"
     "# which is also the rollback. Candidate hashes are not here: stage-kmd174.py freezes",
     "# HOST-TEST FIXTURE (174 over 173). stage.py freeze replaces this file in every attempt with one generated from\n"
     "# the candidate and rollback package manifests and native-caps001/lab-baseline.json. The generated file is the\n"
     "# only place that names a promotion. Candidate hashes are not here: stage.py freezes"),
    ("$KmdCandidateMode='candidate174'", "$KmdCandidateMode='rehearsal'"),
    ("$KmdSameMode='same173'", "$KmdSameMode='same'"),
    ("$KmdDeployMode='deploy174'", "$KmdDeployMode='deploy'"),
    ("$KmdTaskName='BC250-KMD174-Watch'", "$KmdTaskName='BC250-KMD-Watch'"),
])
sub(T / 'transition-policy.ps1', [("'candidate174','same173','deploy174'", "'rehearsal','same','deploy'")])
sub(T / 'confirmed-present-start.ps1', [
    ("[ValidatePattern('^0x[0-9A-F]{8}$')][string]$Abi='0x000700AE')",
     "[string]$Abi=$KmdCandidateAbi)\n if($Abi -notmatch '^0x[0-9A-F]{8}$'){throw 'ABI required: pass -Abi or dot-source identity.ps1 first'}"),
])
sub(T / 'test-identity.ps1', [
    ("[ValidateSet('candidate174','same173','deploy174')]", "[ValidateSet('rehearsal','same','deploy')]"),
    ("$exempt=@('identity.ps1','confirmed-present-start.ps1')", "$exempt=@('identity.ps1')"),
    ("Must-Reject {Get-KmdTransitionPhases candidate 'deploy173'}", "Must-Reject {Get-KmdTransitionPhases candidate 'deploy173'}\nMust-Reject {Get-KmdTransitionPhases candidate 'candidate'}"),
    ("# No version, ABI, package label or historical path literal outside identity.ps1 (tests and the\n"
     "# self-contained gate aside), no leftover of the 170-173 trees and no old desktop UMD pin.",
     "# No version, ABI, package label, hash or lab path literal outside identity.ps1 (tests aside): the template\n"
     "# serves every KMD revision, and each attempt's generated identity.ps1 is its only version-specific file."),
    ("$stale='0x000700A[0-9A-F]|0\\.7\\.1[0-9][0-9]\\.|\\b(candidate|rollback|deploy|same)1[0-9][0-9]\\b|m13\\\\|kmd17[0-3]|KMD17[0-3]|preflight17[01]|postflight173|exact17[0-3]|65172CA1|67F02415|91DDEFDF|0E8C218E|resource-close|8279AC7F|4176D1DF|CF3948D6'",
     "$stale='(?i)0x0007[0-9A-F]{4}|0\\.7\\.[0-9]+\\.[0-9]|\\b(candidate|rollback|deploy|same)[0-9]{3}\\b|m13\\\\|kmd1(?!68)[0-9]{2}|(pre|post)flight[0-9]|exact1[0-9]{2}|[0-9A-F]{64}|resource-close|desktop-umd[0-9]|wsi-final'"),
])
# The directory-guard test uses the fixture's revision; keep its literals, they live in a test file.
sub(T / 'test-transition-policy.ps1', [("mode='deploy173'", "mode='deploy174'"), ("mode='same172'", "mode='candidate'")])
sub(K / 'ops/stage-package.ps1', [
    ("# Runs on unit A through stage-kmd174.py push", "# Runs on unit A through stage.py push"),
    ("# 173 baseline, then", "# rollback baseline, then"),
    ("'^kmd174-deploy[0-9]{3}$'", "'^kmd[0-9]{3}-deploy[0-9]{3}$'"),
])
sub(K / 'ops/postflight-run.ps1', [
    ("postflight-kmd174.py", "postflight.py"),
    ("'^kmd174-deploy[0-9]{3}$'", "'^kmd[0-9]{3}-deploy[0-9]{3}$'"),
])
sub(K / 'ops/fresh-dwm.ps1', [("'^BC250-(KMD|UMD)17[34]-Watch$'", "'^BC250-(KMD|UMD)(17[34])?-Watch$'")])
sub(K / 'ops/inspect-until-done.ps1', [("'dispatch-kmd174.py'", "'dispatch.py'")])
print('ported')
