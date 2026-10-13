"""PASS-only gate and executable cache; explicit inputs, isolated processes, bounded DAG.

Each key hashes content, not timestamps. Declarations can over-approximate a
check's reads; an undeclared check always runs. Full/release runs bypass both
cache reads and writes. Output bytes are verified before restoring a cached PASS.
"""
import argparse
import concurrent.futures
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import uuid

SCHEMA = 1


def digest(data):
    return hashlib.sha256(data).hexdigest()


def encoded(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True).encode()


def normalized(value, out):
    """Output directories are worker-owned, not semantic check inputs."""
    if isinstance(value, str):
        return value.replace(str(out), '<GATE_OUT>').replace(str(out).replace('\\', '/'), '<GATE_OUT>')
    if isinstance(value, dict):
        return {k: normalized(v, out) for k, v in value.items()}
    if isinstance(value, list):
        return [normalized(v, out) for v in value]
    return value


def file_digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix='receipt-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(encoded(value))
        os.replace(name, path)
    finally:
        Path(name).unlink(missing_ok=True)


def contained(root, relative):
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or path == root.resolve():
        raise ValueError('Cache artifact is outside its root')
    return path


class Snapshot:
    def __init__(self, roots):
        self.roots = roots
        self.files = {}
        self.groups = {}

    def hash(self, path):
        name = str(path.resolve())
        if name not in self.files:
            self.files[name] = file_digest(path)
        return self.files[name]

    def group(self, declaration):
        signature = encoded(declaration)
        if signature in self.groups:
            return self.groups[signature]
        rows = []
        for item in declaration:
            root = self.roots[item['root']]
            patterns = item['globs']
            for pattern in patterns:
                # Membership is an input too: additions, removals and renames
                # invalidate the key, including a newly shadowing header.
                files = sorted(p for p in root.glob(pattern) if p.is_file()
                               and not any(x in ('.git', '__pycache__', '.pytest_cache') for x in p.relative_to(root).parts)
                               and p.relative_to(root).as_posix() not in item.get('exclude', []))
                rows.append([item['root'], pattern, [[p.relative_to(root).as_posix(), self.hash(p)] for p in files]])
                if not files and not item.get('optional', False):
                    raise ValueError('Missing declared input: ' + item['root'] + '/' + pattern)
        result = digest(encoded(rows))
        self.groups[signature] = result
        return result


class WindowsJob:
    """Kill descendants at a timeout or parent exit, even if their shell exits."""
    def __init__(self, process):
        self.handle = None
        if os.name != 'nt':
            return
        class Basic(ctypes.Structure):
            _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                        ('flags', wintypes.DWORD), ('min_ws', ctypes.c_size_t), ('max_ws', ctypes.c_size_t),
                        ('active', wintypes.DWORD), ('affinity', ctypes.c_size_t),
                        ('priority', wintypes.DWORD), ('scheduling', wintypes.DWORD)]
        class Io(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in ('read_ops', 'write_ops', 'other_ops', 'read', 'write', 'other')]
        class Limits(ctypes.Structure):
            _fields_ = [('basic', Basic), ('io', Io), ('process_memory', ctypes.c_size_t),
                        ('job_memory', ctypes.c_size_t), ('peak_process', ctypes.c_size_t), ('peak_job', ctypes.c_size_t)]
        api = ctypes.WinDLL('kernel32', use_last_error=True)
        api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        api.CreateJobObjectW.restype = wintypes.HANDLE
        api.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        api.CloseHandle.argtypes = [wintypes.HANDLE]
        self.api = api
        handle = api.CreateJobObjectW(None, None)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        limits = Limits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, winnt.h
        if not api.SetInformationJobObject(handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            api.CloseHandle(handle)
            raise ctypes.WinError(ctypes.get_last_error())
        if not api.AssignProcessToJobObject(handle, wintypes.HANDLE(int(process._handle))):
            api.CloseHandle(handle)
            raise ctypes.WinError(ctypes.get_last_error())
        self.handle = handle

    def close(self):
        if self.handle:
            self.api.CloseHandle(self.handle)
            self.handle = None


def run_process(command, cwd, env, log, timeout):
    with log.open('wb') as output:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT)
        job = None
        try:
            job = WindowsJob(process)
            return process.wait(timeout=timeout), False
        except subprocess.TimeoutExpired:
            if job:
                job.close()
            process.kill()
            process.wait()
            return None, True
        except BaseException:
            process.kill()
            process.wait()
            raise
        finally:
            if job:
                job.close()


def cache_key(job, declaration, input_hash, tool_hash, env_hash, context):
    return digest(encoded({'schema': SCHEMA, 'job': job, 'declaration': declaration,
                           'inputs': input_hash, 'toolchain': tool_hash, 'environment': env_hash,
                           'context': context, 'runner': file_digest(__file__),
                           'worker': file_digest(Path(__file__).with_name('gate-worker.ps1'))}))


def load_pass(cache, key, out):
    entry = cache / key
    try:
        receipt = json.loads((entry / 'pass.json').read_bytes())
        if receipt['key'] != key or receipt['status'] != 'PASS' or receipt['schema'] != SCHEMA:
            return None
        files = receipt['artifacts']
        if not files or len({row['path'] for row in files}) != len(files):
            return None
        # Verify every byte before materializing any output or reporting PASS.
        for row in files:
            src = contained(entry / 'files', row['path'])
            contained(out, row['path'])
            if file_digest(src) != row['sha256']:
                return None
        for row in files:
            src, dst = contained(entry / 'files', row['path']), contained(out, row['path'])
            dst.parent.mkdir(parents=True, exist_ok=True)
            if row['path'] == 'kmd/compile_commands.json':
                def relocate(value):
                    if isinstance(value, str):
                        return value.replace(receipt['output_root'], str(out))
                    if isinstance(value, list):
                        return [relocate(x) for x in value]
                    if isinstance(value, dict):
                        return {k: relocate(v) for k, v in value.items()}
                    return value
                dst.write_text(json.dumps(relocate(json.loads(src.read_text(encoding='utf-8-sig')))), encoding='utf-8')
            else:
                shutil.copyfile(src, dst)
        return receipt
    except (OSError, ValueError, KeyError, TypeError):
        return None


def save_pass(cache, key, out, name, patterns):
    cache.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='publish-', dir=cache))
    try:
        paths = {out / (name + '.log')}
        for pattern in patterns:
            paths.update(p for p in out.glob(pattern) if p.is_file())
        rows = []
        for src in sorted(paths):
            relative = src.relative_to(out).as_posix()
            dst = contained(stage / 'files', relative)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            rows.append({'path': relative, 'sha256': file_digest(dst)})
        atomic_json(stage / 'pass.json', {'schema': SCHEMA, 'key': key, 'status': 'PASS',
                                         'output_root': str(out), 'artifacts': rows})
        destination = cache / key
        if destination.exists() and not load_pass(cache, key, stage / 'verify'):
            # Repair a corrupt entry; rename within the cache before removal so
            # readers see a complete old entry or a miss, never a partial one.
            retired = contained(cache, 'retired-' + uuid.uuid4().hex)
            try:
                destination.rename(retired)
                shutil.rmtree(retired)
            except FileNotFoundError:
                pass
        if not destination.exists():
            try:
                stage.rename(destination)
            except FileExistsError:
                pass  # another producer won; readers still verify its bytes
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def execute(plan, manifest, plan_path, no_cache=False, jobs=None):
    start = time.monotonic()
    variables = plan['variables']
    out = Path(variables['Out']).resolve()
    out.mkdir(parents=True, exist_ok=True)
    repo, workspace = Path(variables['repo']).resolve(), Path(variables['Workspace']).resolve()
    if out.is_relative_to(repo):
        raise ValueError('Gate output must be outside the repository')
    no_cache = no_cache or variables['Profile'] == 'Full' or os.environ.get('BC250_GATE_NOCACHE') == '1'
    cache = Path(os.environ.get('BC250_GATE_CACHE', str(workspace / 'scratch/quality/gate-cache'))).resolve()
    if cache.is_relative_to(repo):
        raise ValueError('BC250_GATE_CACHE must be outside the repository')
    roots = {'repo': repo, 'workspace': workspace, 'kits': Path(variables['Kits']).resolve(),
             'compiler': Path(plan['compiler']).resolve()}
    snapshot = Snapshot(roots)
    specs = {s['name']: s for s in plan['jobs']}
    if len(specs) != len(plan['jobs']):
        raise ValueError('Duplicate gate names')
    release_only = set(manifest['release_only'])
    if variables['Profile'] == 'KmdPackage':
        specs = {n: s for n, s in specs.items() if n not in release_only}
    count = jobs or os.cpu_count() or 1
    if count < 1:
        raise ValueError('Jobs must be positive')
    # The worker controls output roots. Values are hashed, never written to the
    # receipt, so an unrelated secret in the parent environment is not exported.
    environment = dict(os.environ)
    environment.update(BC250_ROOT=str(workspace), PYTHONDONTWRITEBYTECODE='1')
    env_inputs = {k: v for k, v in environment.items() if k.upper() not in
                  {'TEMP', 'TMP', 'BC250_GATE_CACHE', 'BC250_GATE_NOCACHE', 'BC250_QUALITY_OUT'}}
    env_hash = digest(encoded(env_inputs))
    tool_hash = ''
    if not no_cache:
        print('Hashing declared toolchain inputs', flush=True)
        tool_hash = snapshot.group(manifest['toolchain'])
        identities = []
        for tool in ('python', 'pwsh', 'powershell', 'git'):
            executable = shutil.which(tool)
            if not executable:
                raise ValueError('Tool unavailable: ' + tool)
            identities.append([executable, snapshot.hash(Path(executable))])
        # cl invoked through PATH can use the installed SDK through INCLUDE/LIB
        # as well as the explicitly selected NuGet kit. Hash both lookup trees.
        environment_tools = []
        for variable in manifest.get('toolchain_environment', []):
            for directory in environment.get(variable, '').split(os.pathsep):
                if not directory:
                    continue
                root = Path(directory).resolve()
                alias = 'environment-tool-' + digest(str(root).encode())
                roots[alias] = root
                environment_tools.append({'root': alias, 'globs': ['**/*'], 'optional': True})
        inherited_hash = snapshot.group(environment_tools)
        os_version = tuple(sys.getwindowsversion()) if os.name == 'nt' else sys.platform
        tool_hash = digest(encoded([tool_hash, inherited_hash, identities, sys.version, os_version]))
        print(f'Toolchain identity ready in {time.monotonic() - start:.2f}s', flush=True)
    context = normalized({k: v for k, v in variables.items() if k != 'Out'}, out)
    prepared, uncacheable = {}, {}
    for name, spec in specs.items():
        declaration = manifest['checks'].get(name)
        if not declaration or 'inputs' not in declaration:
            uncacheable[name] = 'undeclared inputs'
            prepared[name] = None
        elif no_cache:
            prepared[name] = None
        else:
            try:
                inputs = [item for group in declaration['inputs'] for item in manifest['groups'][group]]
                h = snapshot.group(inputs)
                prepared[name] = cache_key(normalized(spec, out), declaration, h, tool_hash, env_hash, context)
            except (ValueError, OSError) as exc:
                uncacheable[name] = str(exc)
                prepared[name] = None
    for name, reason in uncacheable.items():
        print(f'uncached {name}: {reason}', flush=True)
    preparation_seconds = time.monotonic() - start
    print(f'gates: {len(specs)} checks, {count} workers, cache {"disabled" if no_cache else "enabled"}', flush=True)
    results = [r for r in plan.get('skips', []) if r['name'] not in specs
               and (variables['Profile'] != 'KmdPackage' or r['name'] not in release_only)]
    completed, pending, running = {}, dict(specs), {}

    def work(spec):
        name = spec['name']
        watch = time.monotonic()
        key = prepared[name]
        declaration = manifest['checks'].get(name, {})
        row = {'name': name, 'status': 'FAIL', 'cached': False, 'key': key}
        if key and load_pass(cache, key, out):
            row.update(status='PASS', cached=True, seconds=time.monotonic() - watch)
            print(f'{name} cached PASS {key[:12]}', flush=True)
            return row
        scratch = out / '_work' / name
        scratch.mkdir(parents=True, exist_ok=True)
        env = environment | {'TEMP': str(scratch), 'TMP': str(scratch),
                              'BC250_GATE_NOCACHE': '1' if no_cache else '0'}
        if spec.get('runner'):
            # Direct -File keeps PowerShell's native exit contract and avoids an
            # extra shell for each compiled host suite.
            command = ['pwsh', '-NoProfile', '-File', str(contained(repo, spec['runner'])),
                       *spec.get('arguments', []), '-Out', str(out / name)]
        else:
            command = ['pwsh', '-NoProfile', '-File', str(repo / 'tools/quality/gate-worker.ps1'),
                       '-Plan', str(plan_path), '-Name', name]
        timeout = declaration.get('timeout', manifest['timeout'])
        code, timed_out = run_process(command, scratch, env, out / (name + '.log'), timeout)
        log = (out / (name + '.log')).read_bytes()
        passed = not timed_out and (code == 0 if not spec.get('fails') else code not in (None, 0) and b'FAIL' in log)
        row.update(status='PASS' if passed else 'FAIL', exit_code=code, timeout=timed_out,
                   seconds=time.monotonic() - watch)
        if timed_out:
            row['error'] = f'Check exceeded {timeout}s; process tree stopped'
        print(f'{name} {row["status"]} {row["seconds"]:.2f}s', flush=True)
        return row

    with concurrent.futures.ThreadPoolExecutor(max_workers=count) as pool:
        while pending or running:
            for name in list(pending):
                deps = manifest['checks'].get(name, {}).get('depends', [])
                if any(d not in specs for d in deps):
                    raise ValueError('Unknown dependency for ' + name)
                if not all(d in completed for d in deps):
                    continue
                if any(completed[d]['status'] != 'PASS' for d in deps):
                    row = {'name': name, 'status': 'FAIL', 'error': 'prerequisite failed', 'cached': False}
                    completed[name] = row
                    results.append(row)
                    del pending[name]
                    continue
                if len(running) == count:
                    break
                spec = pending.pop(name)
                running[pool.submit(work, spec)] = name
            if not running:
                if pending:
                    raise ValueError('Dependency cycle')
                break
            done, _ = concurrent.futures.wait(running, return_when=concurrent.futures.FIRST_COMPLETED)
            for future in done:
                name = running.pop(future)
                try:
                    row = future.result()
                except Exception as exc:
                    row = {'name': name, 'status': 'FAIL', 'cached': False, 'error': str(exc)}
                    print(f'{name} FAIL: {exc}', flush=True)
                results.append(row)
                completed[name] = row
    # Rehash the declaration groups before publishing. Never bless a run over
    # moving sources (nor reuse such a run as a later cache hit).
    stable = True
    if not no_cache:
        after = Snapshot(roots)
        for declaration, old in snapshot.groups.items():
            if after.group(json.loads(declaration)) != old:
                stable = False
                break
        if not stable:
            results.append({'name': 'input-stability', 'status': 'FAIL', 'error': 'Inputs changed during gates'})
        else:
            for row in results:
                key = row.get('key')
                if key and row['status'] == 'PASS' and not row.get('cached'):
                    declaration = manifest['checks'][row['name']]
                    artifacts = declaration.get('artifacts', [row['name'] + '/**/*.exe', row['name'] + '/**/*.json'])
                    save_pass(cache, key, out, row['name'], artifacts)
    report = {'status': 'PASS' if all(r['status'] in ('PASS', 'SKIP') for r in results) else 'FAIL',
              'profile': variables['Profile'], 'kits': variables['Kits'], 'context': plan['context'],
              'seconds': time.monotonic() - start, 'checks': results, 'cache_disabled': no_cache,
              'cache_hits': sum(bool(r.get('cached')) for r in results), 'uncacheable': uncacheable,
              'preparation_seconds': preparation_seconds,
              'moved_to_full': sorted(release_only) if variables['Profile'] == 'KmdPackage' else [],
              'workers': count}
    atomic_json(out / 'result.json', report)
    print(f'{report["status"]}: {report["seconds"]:.2f}s, {report["cache_hits"]} cache hits', flush=True)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, default=Path(__file__).with_name('gate-inputs.json'))
    parser.add_argument('--no-cache', action='store_true')
    parser.add_argument('--jobs', type=int)
    args = parser.parse_args()
    plan = json.loads(args.plan.read_text(encoding='utf-8-sig'))
    manifest = json.loads(args.manifest.read_text(encoding='utf-8-sig'))
    try:
        report = execute(plan, manifest, args.plan.resolve(), args.no_cache, args.jobs)
    except Exception as exc:
        report = {'status': 'FAIL', 'error': str(exc)}
        atomic_json(Path(plan['variables']['Out']) / 'result.json', report)
        print('FAIL: ' + str(exc), flush=True)
    return 0 if report['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
