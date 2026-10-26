"""Bounded static analysis with the compiler from the actual build commands.

MSVC results use dependency caching. Clang results always execute the analyzer.
"""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
import plistlib
from pathlib import Path
import re
import shutil
import subprocess
import time


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def split_command(command):
    shell = ctypes.windll.shell32
    shell.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
    n = ctypes.c_int()
    ptr = shell.CommandLineToArgvW(command, ctypes.byref(n))
    if not ptr:
        raise ValueError('Cannot parse compiler command')
    try:
        return [ptr[i] for i in range(n.value)]
    finally:
        ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.LocalFree(ptr)


def clang_analysis_command(compiler, argv, report):
    # Preserve the build's target, defines, includes and warning policy. The
    # analyzer emits a plist instead of the build's object/PDB or include trace.
    filtered = [x for x in argv[1:] if not x.lower().startswith(('/fo', '/fd', '/analyze'))
                and x.lower() not in ('/c', '-c', '/showincludes', '--analyze')]
    return [str(compiler), *filtered, '--analyze', '-Xclang', '-analyzer-output=plist-multi-file',
            '-o', str(report)]


def clang_report_passed(code, log, report, baseline=None, cwd=None, source=None):
    if code or re.search(rb'\berror:', log):
        return False
    if not report.is_file():
        return False
    data = plistlib.loads(report.read_bytes())
    diagnostics = data.get('diagnostics')
    if not isinstance(diagnostics, list):
        return False
    # Every textual warning must also have a structured diagnostic. In particular,
    # ignored arguments and incomplete analyzer reports cannot be baselined.
    if len(re.findall(rb'\bwarning:', log)) != len(diagnostics):
        return False
    if not diagnostics:
        return True
    if not baseline or not cwd or not source:
        return False
    if digest(source) != baseline['translation_unit_sha256']:
        return False
    for diagnostic in diagnostics:
        location = diagnostic['location']
        path = (cwd / data['files'][location['file']]).resolve()
        actual = {key: diagnostic[key] for key in ('check_name', 'description', 'issue_context',
                                                   'issue_hash_content_of_line_in_context')}
        actual.update(line=location['line'], col=location['col'], file_sha256=digest(path))
        if not any(all(row.get(k) == v for k, v in actual.items()) and row.get('reason')
                   for row in baseline['diagnostics']):
            return False
    return True


def run(args):
    args.out = args.out.resolve()
    db = json.loads(args.database.read_text(encoding='utf-8-sig'))
    selected = [e for e in db if re.search(args.match, e['file'].replace('\\', '/'))]
    if not selected:
        raise ValueError('No compilation commands matched')
    args.out.mkdir(parents=True, exist_ok=True)
    results = []
    for entry in selected:
        start = time.monotonic()
        cwd = Path(entry['directory'])
        argv = entry.get('arguments') or split_command(entry['command'])
        compiler = Path(shutil.which(argv[0]) or argv[0]).resolve()
        if compiler.name.lower() not in ('cl.exe', 'clang-cl.exe'):
            raise ValueError('Expected cl.exe or clang-cl.exe, found '+str(compiler))
        source = (cwd / entry['file']).resolve()
        ident = hashlib.sha256(str(source).encode()).hexdigest()[:16]
        out = args.out / ident
        out.mkdir(exist_ok=True)
        record = out / 'pass.json'
        if compiler.name.lower() == 'clang-cl.exe':
            record.unlink(missing_ok=True)
            report = out / 'analysis.plist'
            report.unlink(missing_ok=True)
            cmd = clang_analysis_command(compiler, argv, report)
            before = digest(source)
            try:
                result = subprocess.run(cmd, cwd=cwd, capture_output=True, timeout=args.timeout)
            except subprocess.TimeoutExpired as exc:
                (out / 'analysis.log').write_bytes((exc.stdout or b'') + (exc.stderr or b''))
                raise RuntimeError('Analysis deadline: ' + str(source))
            log = result.stdout + result.stderr
            (out / 'analysis.log').write_bytes(log)
            baseline = json.loads(args.clang_baseline.read_text()) if args.clang_baseline else None
            if not clang_report_passed(result.returncode, log, report, baseline, cwd, source):
                raise RuntimeError('Clang analysis failed: ' + str(source) + '; ' + str(out / 'analysis.log'))
            if before != digest(source):
                raise RuntimeError('Source changed during analysis')
            record.write_text(json.dumps({'compiler': str(compiler), 'compiler_sha256': digest(compiler),
                                         'source_sha256': before, 'command': cmd,
                                         'report_sha256': digest(report), 'cached': False,
                                         'baseline_sha256': digest(args.clang_baseline) if baseline else None}, indent=2))
            results.append({'file': str(source), 'status': 'PASS',
                            'seconds': round(time.monotonic() - start, 3)})
            continue
        # Include lookup order and new shadowing headers are inputs too.
        include_dirs = [x[2:].strip('"') for x in argv[1:] if x.lower().startswith('/i') or x.startswith('-I')]
        include_dirs += os.environ.get('INCLUDE', '').split(';')
        names = []
        for directory in sorted(set(include_dirs)):
            if not directory:
                continue
            root = (cwd / directory).resolve()
            if root.is_dir():
                names += [str(p) for p in root.rglob('*') if p.is_file() and p.suffix.lower() in ('.h','.hpp','.inl','.inc')]
        tools = [compiler] + sorted(compiler.parent.glob('*.dll'))
        signature = hashlib.sha256(json.dumps({
            'argv':argv, 'cwd':str(cwd), 'checker':digest(__file__),
            'tools':{str(p):digest(p) for p in tools},
            'include':os.environ.get('INCLUDE',''), 'headers':sorted(set(names)),
        },sort_keys=True).encode()).hexdigest()
        if record.exists() and not args.force and os.environ.get('BC250_GATE_NOCACHE') != '1':
            old = json.loads(record.read_text())
            if old['signature'] == signature and all(Path(p).is_file() and digest(p)==h for p,h in old['dependencies'].items()):
                results.append({'file':str(source),'status':'CACHED','seconds':round(time.monotonic()-start,3)})
                continue
        record.unlink(missing_ok=True)
        # Keep all original defines/include paths and warning choices, redirect outputs.
        filtered = [x for x in argv[1:] if not x.lower().startswith(('/fo','/fd','/analyze'))]
        cmd = [str(compiler), *filtered, '/analyze', '/we4013', '/we4020', '/we4024',
               '/sourceDependencies',str(out/'dependencies.json'),'/Fo'+str(out/'analysis.obj'),'/Fd'+str(out/'analysis.pdb')]
        before = digest(source)
        try:
            result = subprocess.run(cmd,cwd=cwd,capture_output=True,timeout=args.timeout)
        except subprocess.TimeoutExpired as exc:
            (out/'analysis.log').write_bytes((exc.stdout or b'')+(exc.stderr or b''))
            raise RuntimeError('Analysis deadline: '+str(source))
        log = result.stdout+result.stderr
        (out/'analysis.log').write_bytes(log)
        if result.returncode or re.search(rb'\bwarning C(?:6\d{3}|28\d{3})\b',log):
            raise RuntimeError('Analysis failed: '+str(source)+'; '+str(out/'analysis.log'))
        deps = json.loads((out/'dependencies.json').read_text(encoding='utf-8-sig'))['Data']
        paths = set(deps.get('Includes', [])) | {str(source)}
        hashes = {str(Path(p).resolve()):digest(p) for p in paths}
        if before != hashes[str(source)]:
            raise RuntimeError('Source changed during analysis')
        record.write_text(json.dumps({'signature':signature,'dependencies':hashes,'command':cmd},indent=2))
        results.append({'file':str(source),'status':'PASS','seconds':round(time.monotonic()-start,3)})
    (args.out/'summary.json').write_text(json.dumps(results,indent=2))
    for row in results:
        print(row['status'],row['seconds'],Path(row['file']).name)


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--database',type=Path,required=True)
    p.add_argument('--match',required=True)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--timeout',type=int,default=60)
    p.add_argument('--force',action='store_true')
    p.add_argument('--clang-baseline', type=Path)
    run(p.parse_args())
