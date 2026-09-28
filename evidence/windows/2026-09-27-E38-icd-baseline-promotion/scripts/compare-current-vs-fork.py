"""Compare Codex's lab ICD source tree (scratch/m12/mesa-current-src, working tree on 05e6c962, ICD 3508416F)
with the fork branch tree at a given commit (worktree wt-radv). Read-only. Normalises CRLF and the
double-encoded copyright sign. Prints files that differ or exist on one side only, restricted to src/ and
meson.build, over the union of files either side changed relative to upstream 05e6c962."""
import subprocess, sys, pathlib, difflib

cur = pathlib.Path('P:/BC-250/scratch/m12/mesa-current-src')
fork = pathlib.Path('P:/BC-250/scratch/mesa-fork-2026-09-27/wt-radv')
rev = sys.argv[1] if len(sys.argv) > 1 else '940ab0eb'
base = '05e6c962'

def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo)] + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace').stdout

def norm(b):
    if b is None:
        return None
    return b.replace(b'\r\n', b'\n').replace(b'\xc3\x82\xc2\xa9', b'\xc2\xa9')

def blob(repo, rev, path):
    r = subprocess.run(['git', '-C', str(repo), 'show', f'{rev}:{path}'], capture_output=True)
    return r.stdout if r.returncode == 0 else None

cur_changed = set(l.split('\t')[-1].strip() for l in git(cur, 'diff', '--name-status', base).splitlines() if l.strip())
cur_untracked = set(l.strip() for l in git(cur, 'ls-files', '--others', '--exclude-standard').splitlines() if l.strip())
fork_changed = set(l.strip() for l in git(fork, 'diff', '--name-only', base, rev).splitlines() if l.strip())

def interesting(p):
    return p == 'meson.build' or p.startswith('src/')

files = sorted(p for p in (cur_changed | cur_untracked | fork_changed) if interesting(p))
same = diff = only_cur = only_fork = 0
for p in files:
    f = cur / p
    c = norm(f.read_bytes()) if f.is_file() else None
    k = norm(blob(fork, rev, p))
    if c is None and k is None:
        continue
    if c is None:
        only_fork += 1; print(f'ONLY-FORK  {p}'); continue
    if k is None:
        only_cur += 1; print(f'ONLY-CUR   {p}'); continue
    if c == k:
        same += 1; continue
    diff += 1
    cl = c.decode('utf-8', 'replace').splitlines()
    kl = k.decode('utf-8', 'replace').splitlines()
    d = [l for l in difflib.unified_diff(kl, cl, 'fork', 'current', n=0, lineterm='') if l.startswith(('+', '-')) and not l.startswith(('+++', '---'))]
    print(f'DIFF       {p}  (+{sum(1 for l in d if l[0]=="+")} -{sum(1 for l in d if l[0]=="-")} lines, current vs fork)')
    for l in d[:12]:
        print('    ' + l[:160])
print(f'\nfiles={len(files)} same={same} diff={diff} only_current={only_cur} only_fork={only_fork} fork_rev={rev}')
