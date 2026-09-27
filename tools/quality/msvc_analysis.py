"""Bounded, dependency-cached MSVC analysis of selected actual build commands."""
import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
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


def run(args):
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
        if compiler.name.lower() != 'cl.exe':
            raise ValueError('Expected MSVC cl.exe, found '+str(compiler))
        source = (cwd / entry['file']).resolve()
        ident = hashlib.sha256(str(source).encode()).hexdigest()[:16]
        out = args.out / ident
        out.mkdir(exist_ok=True)
        record = out / 'pass.json'
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
        if record.exists() and not args.force:
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
    run(p.parse_args())
