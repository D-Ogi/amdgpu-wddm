"""The spawn-loop ratchet (BD-051): no new polling loop that starts a process each round.

A polling loop that starts bc250kmd_cli (or any tool) once a second is one half of the BD-051 sshd accept stall: while
it runs inside an ssh session or beside it, sshd completes the TCP handshake of a new connection and sends no banner,
for minutes. The lab tools read the KMD in-process (tools/win/kmdread/kmd-reader.ps1) instead.

What counts:
  PowerShell (.ps1, .psm1): spawn-loop-ast.ps1 parses each file with the PowerShell parser (it runs nothing) and
    reports the innermost loops that both wait (Start-Sleep, Thread::Sleep, .WaitOne, or a function of the file that
    does) and start a process (an application, & on a variable that holds a path, Start-Process, Start-Job,
    Start-ScheduledTask, [Diagnostics.Process]::Start, or a function of the file that does).
  Shell (.sh): a while/until/for loop whose body has a sleep and reaches the lab: ssh, scp, target.py, lab-emerg,
    inp.py, mon.py, channel.py, powershell or pwsh, directly or through a function of the same script.
A loop that must start a process each round says why on its first line or on one of the three lines above it:
"spawn-loop-ok: <reason>". It is reported as allowed and not counted.

Modes:
  spawn_loop_lint.py [--root REPO]                the gate: tracked files (evidence/ excluded) against the baseline
                                                  spawn_loop_baseline.txt; a file may not get more loops than its line
                                                  there says (a file not in it: none). Exit 1 on a breach.
  spawn_loop_lint.py --files P [P ...]            any files (a kit), no baseline: exit 1 on any counted loop
  spawn_loop_lint.py --update-baseline            writes the baseline from the tracked files as they are now
  --list                                          prints every loop found, counted or allowed
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
AST = HERE / 'spawn-loop-ast.ps1'
BASELINE = HERE / 'spawn_loop_baseline.txt'
PS51 = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'WindowsPowerShell' / 'v1.0' / 'powershell.exe'
EXCLUDED = ('evidence/',)
SH_LAB = re.compile(r'\b(ssh|scp|powershell|pwsh)\b|target\.py|lab-emerg|\binp\.py|\bmon\.py|\bchannel\.py')
ALLOW = re.compile(r'spawn-loop-ok:\s*(\S.*)$')


def tracked(root):
    out = subprocess.run(['git', '-C', str(root), 'ls-files', '-z', '--', '*.ps1', '*.psm1', '*.sh'],
                         capture_output=True, check=True).stdout.decode('utf-8')
    return [p for p in out.split('\0') if p and not p.startswith(EXCLUDED)]


def ps_loops(paths, jobs=None):
    """{path: {'error': str, 'loops': [...]}} for PowerShell files, from a few Windows PowerShell 5.1 processes.

    The parser is the slow part (about 70 ms a file), so the list is split into interleaved shares, one process each.
    """
    if not paths:
        return {}
    jobs = max(1, min(jobs or min(6, os.cpu_count() or 1), len(paths)))
    running = []
    try:
        for k in range(jobs):
            with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False, encoding='utf-8',
                                             dir=os.environ.get('BC250_TEST_TMP') or None) as f:
                f.write('\n'.join(str(p) for p in paths[k::jobs]))
            p = subprocess.Popen([str(PS51), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
                                  str(AST), '-List', f.name], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            running.append((p, f.name))
        data = []
        for p, _ in running:
            stdout, stderr = p.communicate(timeout=1800)
            if p.returncode != 0:
                raise RuntimeError('spawn-loop-ast.ps1 failed: ' + stderr.decode('utf-8', 'replace')[-2000:])
            part = json.loads(stdout.decode('utf-8-sig') or '[]')
            data.extend([part] if isinstance(part, dict) else part)
    finally:
        for p, listing in running:
            if p.poll() is None:
                p.kill()
                p.wait()
            os.unlink(listing)
    result = {}
    for entry in data:
        loops = entry.get('loops') or []
        if isinstance(loops, dict):
            loops = [loops]
        for loop in loops:
            starts = loop.get('starts') or []
            loop['starts'] = [starts] if isinstance(starts, dict) else starts
        result[str(Path(entry['path']))] = {'error': entry.get('error') or '', 'loops': loops}
    return result


SH_FUNC = re.compile(r'^\s*(?:function\s+)?([A-Za-z_][\w-]*)\s*\(\s*\)\s*\{?\s*$|^\s*function\s+([A-Za-z_][\w-]*)\s*\{?\s*$')


SH_ASSIGN = re.compile(r'^\s*(?:local\s+|export\s+|readonly\s+)?([A-Za-z_]\w*)=(.*)$')


def sh_lab_rule(lines):
    """SH_LAB, widened by the variables of the script that hold a lab tool (emerg="$here/../lab-emerg.py")."""
    names = set()
    for raw in lines:
        m = SH_ASSIGN.match(raw.split('#', 1)[0])
        if m and SH_LAB.search(m.group(2)):
            names.add(m.group(1))
    if not names:
        return SH_LAB
    alt = '|'.join(re.escape(n) for n in sorted(names))
    return re.compile(SH_LAB.pattern + r'|\$\{?(?:' + alt + r')\b')


def sh_functions(lines, lab_rule=SH_LAB):
    """{name: (reaches the lab, sleeps)} for the functions of a shell script, through the calls between them."""
    bodies, name, depth = {}, None, 0
    for raw in lines:
        code = raw.split('#', 1)[0]
        if name is None:
            m = SH_FUNC.match(code)
            if m:
                name, depth = m.group(1) or m.group(2), code.count('{') - code.count('}')
                bodies[name] = []
            continue
        bodies[name].append(code)
        depth += code.count('{') - code.count('}')
        if depth <= 0 and re.match(r'^\s*\}', code):
            name = None
    lab = {n: any(lab_rule.search(t) for t in b) for n, b in bodies.items()}
    sleeps = {n: any(re.search(r'\bsleep\b', t) for t in b) for n, b in bodies.items()}
    changed = True
    while changed:
        changed = False
        for n, b in bodies.items():
            for other in bodies:
                if other != n and any(re.search(r'(^|[\s;&|(`$])' + re.escape(other) + r'\b', t) for t in b):
                    if lab[other] and not lab[n]:
                        lab[n] = changed = True
                    if sleeps[other] and not sleeps[n]:
                        sleeps[n] = changed = True
    return {n: (lab[n], sleeps[n]) for n in bodies}


def sh_loops(path):
    """The polling loops of a shell script that reach the lab each round, directly or through its functions."""
    lines = Path(path).read_text(encoding='utf-8', errors='replace').splitlines()
    lab_rule = sh_lab_rule(lines)
    funcs = sh_functions(lines, lab_rule)
    calls = {n: re.compile(r'(^|[\s;&|(`$])' + re.escape(n) + r'\b') for n in funcs}

    def lab_starts(n, t):
        if SH_ASSIGN.match(t) and not re.search(r'\$\(|`', t):
            return []
        m = lab_rule.search(t)
        if m:
            return [{'line': n, 'what': m.group(0)}]
        return [{'line': n, 'what': f + '()'} for f, (lab, _) in funcs.items() if lab and calls[f].search(t)]

    def waits(t):
        return bool(re.search(r'\bsleep\b', t)) or any(s and calls[f].search(t) for f, (_, s) in funcs.items())

    loops, stack = [], []
    for i, raw in enumerate(lines, 1):
        code = raw.split('#', 1)[0] if not raw.lstrip().startswith('#!') else ''
        for word in re.findall(r'\b(while|until|for|do|done)\b', code):
            if word in ('while', 'until', 'for'):
                stack.append({'line': i, 'body': [], 'opened': False})
            elif word == 'do' and stack:
                stack[-1]['opened'] = True
            elif word == 'done' and stack:
                loop = stack.pop()
                body = loop['body'] + [(i, code)]
                inner = [b for b in loops if loop['line'] < b['line'] <= i]
                if inner:
                    continue
                waited = any(waits(t) for _, t in body)
                starts = [s for n, t in body for s in lab_starts(n, t)]
                if waited and starts:
                    allowed = ''
                    for j in range(max(1, loop['line'] - 3), loop['line'] + 1):
                        m = ALLOW.search(lines[j - 1])
                        if m:
                            allowed = m.group(1).strip()
                    loops.append({'line': loop['line'], 'kind': 'shell', 'allowed': allowed, 'starts': starts})
        for loop in stack:
            loop['body'].append((i, code))
    return loops


def scan(paths):
    """{path: {'error', 'loops'}} for the given paths, PowerShell and shell."""
    ps = [p for p in paths if str(p).lower().endswith(('.ps1', '.psm1'))]
    result = ps_loops(ps)
    for p in paths:
        if str(p).lower().endswith('.sh'):
            result[str(Path(p))] = {'error': '', 'loops': sh_loops(p)}
    return result


def counted(entry):
    return [l for l in entry['loops'] if not l.get('allowed')]


def read_baseline(path=BASELINE):
    base = {}
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.startswith('#'):
                continue
            n, rel = line.split('\t', 1)
            base[rel.strip()] = int(n)
    return base


def write_baseline(counts, path=BASELINE):
    head = ['# spawn-loop ratchet baseline (BD-051): the polling loops that start a process each round, a file a line,',
            '# as they were when the gate landed (2026-11-02). tools/quality/spawn_loop_lint.py reads this file, and',
            '# tools/quality/quick.ps1 runs the gate. One line a file: <loops><tab><path>. A file may not get more',
            '# loops than its line says; a file that is not here may have none. Lower a count when a loop goes away',
            '# (spawn_loop_lint.py --update-baseline). The rule and the allow comment: spawn_loop_lint.py.']
    rows = ['%d\t%s' % (n, rel) for rel, n in sorted(counts.items()) if n]
    path.write_text('\n'.join(head + rows) + '\n', encoding='utf-8', newline='\n')


def show(rel, entry, out):
    for loop in entry['loops']:
        what = ', '.join('%s (line %s)' % (s['what'], s['line']) for s in loop['starts'][:4])
        state = 'allowed: ' + loop['allowed'] if loop.get('allowed') else 'counted'
        out.write('  %s:%s %s loop, %s; starts %s\n' % (rel, loop['line'], loop['kind'], state, what))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--root', default=str(HERE.parents[1]))
    ap.add_argument('--files', nargs='+')
    ap.add_argument('--update-baseline', action='store_true')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--baseline', default=str(BASELINE))
    a = ap.parse_args(argv)
    out = sys.stdout
    if a.files:
        result = scan([Path(p) for p in a.files])
        bad = 0
        for rel, entry in sorted(result.items()):
            if entry['error']:
                out.write('ERROR %s: %s\n' % (rel, entry['error']))
                bad += 1
            if a.list or counted(entry):
                show(rel, entry, out)
            bad += len(counted(entry))
        out.write('spawn-loop: %d file(s), %d counted loop(s)\n' % (len(result), bad))
        return 1 if bad else 0

    root = Path(a.root)
    rels = tracked(root)
    result = scan([root / r for r in rels])
    by_rel = {r: result.get(str(root / r), {'error': '', 'loops': []}) for r in rels}
    counts = {r: len(counted(e)) for r, e in by_rel.items()}
    if a.update_baseline:
        write_baseline(counts, Path(a.baseline))
        out.write('spawn-loop: baseline written, %d file(s) with loops, %d loop(s)\n'
                  % (sum(1 for n in counts.values() if n), sum(counts.values())))
        return 0
    base = read_baseline(Path(a.baseline))
    breaches, lower, errors = [], [], []
    for rel, e in sorted(by_rel.items()):
        if e['error'] and not e['error'].startswith('parse'):
            errors.append((rel, e['error']))
        n, allowed = counts[rel], base.get(rel, 0)
        if n > allowed:
            breaches.append(rel)
            out.write('FAIL %s: %d polling loop(s) that start a process, the baseline allows %d\n' % (rel, n, allowed))
            show(rel, e, out)
        elif n < allowed:
            lower.append(rel)
        elif a.list and e['loops']:
            show(rel, e, out)
    for rel, err in errors:
        out.write('ERROR %s: %s\n' % (rel, err))
    for rel in lower:
        out.write('note %s: %d loop(s), the baseline says %d: lower it (--update-baseline)\n' % (rel, counts[rel], base[rel]))
    out.write('spawn-loop: %d file(s), %d loop(s) counted, %d breach(es)\n' % (len(rels), sum(counts.values()), len(breaches)))
    return 1 if breaches or errors else 0


if __name__ == '__main__':
    sys.exit(main())
