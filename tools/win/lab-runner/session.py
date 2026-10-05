"""Game runner session helper for native-caps001/run-game.sh. Offline: nothing here reaches the lab.

    python tools/win/lab-runner/session.py facts PROFILE [--api API] [--kit DIR]
    python tools/win/lab-runner/session.py dry N PROFILE --out DIR [--api API] [--seconds S]
                                           [--launch steam|direct] [--experiment NAMES] [--kit DIR]

facts prints shell assignments (shlex-quoted, nothing else on stdout) for run-game.sh: the profile as run.py's own
loader reads it, the session's API, default seconds, the game's process names (main and renderers), the router rule,
the registry values and benchmark world timing of the API, the app's Steam EULA, and the triplet the session stages. The triplet is run-m157.sh's (its adapter default, SESSION_ENGINE, SESSION_ICD)
unless SESSION_ADAPTER, SESSION_ENGINE or SESSION_ICD are set; GP_ACCEPTED=1 when all three equal the accepted
triplet of lab-baseline.json (a session in place, as run-m157.sh's default). Witcher 3 is refused: its sessions
belong to run-m157.sh.

dry stages the attempt as the session would (run.py Stage with the inputs and environment that run-game.sh ->
run-dpm-game.sh -> run-cache-pair.sh -> run-game-trial.sh -> trial.py -> stage-attempt.py give it) into
DIR/native-capsN (BC250_STAGE_DRY_DIR; run.py refuses the attempts directory), lists the package run-slot.py would
push with the lab-side dispatch commands, prints config.json and runs the staged game-runtime.ps1 -DryRun -Offline
under Windows PowerShell 5.1 (TEMP inside DIR).

--kit names another kit directory at the same depth as native-caps001 (scratch/m15/<kit>), e.g. a test copy.

The trial kits live in the workspace scratch directory, not in this repository: BC250_ROOT names the workspace
root (by default the parent directory of this repository), as tools/win/target.py reads it."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

# This file is bc250-win/tools/win/lab-runner/session.py: the repository root is three levels up, the workspace
# root one more (BC250_ROOT overrides it, as in tools/win/target.py).
REPO = Path(__file__).resolve().parents[3]
ROOT = Path(os.environ.get('BC250_ROOT') or REPO.parent)
LIVE_KIT = ROOT / 'scratch/m15/native-caps001'
# The Witcher 3 session script names the session triplet; run-game.sh follows it.
M157 = LIVE_KIT / 'run-m157.sh'
# The client run-game-trial.sh passes to trial.py (staged only for client trials; a game trial ignores it).
CLIENT = ROOT / 'scratch/m15/interactive-client018/build/amdgpu_wddm_d3d12_queue.exe'
POWERSHELL = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest().upper()


def load_run(kit):
    if kit.parent != ROOT / 'scratch/m15' or not (kit / 'run.py').is_file():
        raise SystemExit('kit must be a directory scratch/m15/<name> with run.py: ' + str(kit))
    # run.py imports the Drive planner at import time.
    sys.path.insert(0, str(REPO / 'tools/win/d3d12queue'))
    spec = importlib.util.spec_from_file_location('game_runner_kit_run', kit / 'run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    if not hasattr(module, 'load_game_profile'):
        raise SystemExit(str(kit / 'run.py') + ' predates the game runner (no load_game_profile): install it first')
    return module


def session_triplet():
    """(adapter, shell, engine, icd): run-m157.sh's defaults unless SESSION_ADAPTER/SESSION_ENGINE/SESSION_ICD."""
    text = M157.read_text(encoding='utf-8')
    found = {}
    for key, pattern in (('adapter', r'adapter="\$\{SESSION_ADAPTER:-(adapter[0-9]{3})\}"'),
                         ('engine', r'^SESSION_ENGINE=(\S+) \\$'), ('icd', r'^SESSION_ICD=(\S+) \\$')):
        match = re.search(pattern, text, re.M)
        found[key] = match.group(1) if match else ''
    adapter = os.environ.get('SESSION_ADAPTER') or found['adapter']
    engine = os.environ.get('SESSION_ENGINE') or found['engine']
    icd = os.environ.get('SESSION_ICD') or found['icd']
    if not (adapter and engine and icd):
        raise SystemExit('run-m157.sh no longer names its adapter, SESSION_ENGINE and SESSION_ICD as session.py '
                         'reads them: set SESSION_ADAPTER, SESSION_ENGINE and SESSION_ICD')
    if not re.fullmatch('adapter[0-9]{3}', adapter):
        raise SystemExit('SESSION_ADAPTER must be adapterNNN')
    paths = [ROOT / 'scratch/m15' / adapter / 'amdgpu_wddm_d3d12.dll', Path(engine), Path(icd)]
    for path in paths:
        if not path.is_file():
            raise SystemExit('session artifact missing: ' + str(path))
    return [adapter] + [path.resolve() for path in paths]


def resolve(args):
    # An id, not a path: Stage loads profiles/<id>.json by the id run-game.sh passes on.
    if not re.fullmatch('[a-z0-9][a-z0-9-]{0,63}', args.profile):
        raise SystemExit('PROFILE is a profile id (a file name of this tool's profiles directory without .json)')
    kit = (ROOT / 'scratch/m15' / args.kit) if args.kit else LIVE_KIT
    run = load_run(kit.resolve())
    try:
        _, profile = run.load_game_profile(args.profile)
    except (RuntimeError, ValueError) as error:
        raise SystemExit(str(error))
    if profile['readiness']['mode'] == 'witcher3-menu':
        raise SystemExit(profile['id'] + ': Witcher 3 sessions run through run-m157.sh (presets, resolution and '
                         'upscaler checks)')
    api = args.api or profile['api']
    if api not in profile['apis']:
        raise SystemExit('API %s is not in profile %s (%s)' % (api, profile['id'], ','.join(profile['apis'])))
    adapter, shell, engine, icd = session_triplet()
    hashes = [digest(path) for path in (shell, engine, icd)]
    accepted = run.d3d12_accepted(run.lab_baseline())
    in_place = all(h == accepted[name] for h, name in zip(hashes, run.D3D12_NAMES))
    return kit.resolve(), run, profile, api, dict(adapter=adapter, shell=shell, engine=engine, icd=icd, hashes=hashes,
                                                  in_place=in_place)


def facts(args):
    _, _, profile, api, triplet = resolve(args)
    processes = profile['processes']
    router = profile.get('app_router') or {}
    values = dict(
        GP_ID=profile['id'], GP_TITLE=profile.get('title', profile['id']), GP_APP_ID=str(profile['app_id']),
        GP_API=api, GP_SECONDS=str(profile['session']['default_seconds']),
        GP_PROCESSES=','.join(dict.fromkeys([processes['main']] + processes['renderers'])),
        GP_ROUTER=profile['apis'][api]['router'], GP_ROUTER_IMAGES=','.join(profile.get('router_images', [])),
        GP_ROUTER_ADD=router.get('add', ''), GP_ROUTER_REMOVE=router.get('remove', ''),
        # The game settings the runtime checks before the launch (apis.<api>.registry), for the operator's header.
        GP_REGISTRY='; '.join('%s=%d under HKCU\\%s' % (n['name'], n['value'], n['key'])
                              for n in profile['apis'][api].get('registry', [])),
        # A benchmark's world mark by time (its scenes capture black): the N of note:world+N, sent with its confirm.
        GP_WORLD_AFTER=str(profile['apis'][api].get('world_after_confirm_s', '')),
        GP_EULA=profile['steam'].get('eula', ''),
        # Forward slashes, as run-m157.sh passes them on to run-dpm-game.sh.
        GP_ADAPTER=triplet['adapter'], GP_ENGINE=triplet['engine'].as_posix(), GP_ICD=triplet['icd'].as_posix(),
        GP_TRIPLET=' '.join(h[:8] for h in triplet['hashes']), GP_ACCEPTED='1' if triplet['in_place'] else '')
    for key, value in values.items():
        print('%s=%s' % (key, shlex.quote(value)))


def dry(args):
    if not re.fullmatch('[0-9]{3}', args.n):
        raise SystemExit('N: three digits')
    kit, _, profile, api, triplet = resolve(args)
    out = Path(args.out).resolve()
    attempt = 'native-caps' + args.n
    seconds = args.seconds or profile['session']['default_seconds']
    # The session's Stage environment, nothing inherited from this shell's BC250_* variables.
    env = {k: v for k, v in os.environ.items() if not k.startswith('BC250_')}
    env.update(BC250_STAGE_DRY_DIR=str(out), BC250_TRIAL_GAME='1', BC250_TRIAL_GAME_PROFILE=profile['id'],
               BC250_TRIAL_GAME_API=api, BC250_TRIAL_GAME_SECONDS=str(seconds), BC250_TRIAL_INTERACTIVE='1',
               BC250_TRIAL_DWM='gpu')
    if args.launch == 'steam':
        env['BC250_TRIAL_LAUNCH'] = 'steam'
    if args.experiment:
        env['BC250_TRIAL_EXPERIMENT'] = args.experiment
    if triplet['in_place']:
        env['BC250_TRIAL_ACCEPTED'] = '1'
    command = [sys.executable, str(kit / 'run.py'), 'Stage', '--attempt', attempt]
    for role, path in (('candidate', triplet['shell']), ('engine', triplet['engine']), ('icd', triplet['icd']),
                       ('interactive-client', CLIENT)):
        command += ['--' + role, str(path), '--' + role + '-sha256', digest(path)]
    print('stage env ' + ' '.join('%s=%s' % (k, env[k]) for k in sorted(env) if k.startswith('BC250_')))
    done = subprocess.run(command, cwd=str(ROOT), env=env, capture_output=True, text=True)
    print((done.stdout + done.stderr).strip())
    if done.returncode:
        raise SystemExit('dry Stage failed (exit %d)' % done.returncode)
    pkg = out / attempt / 'package'
    manifest = (out / attempt / 'manifest.sha256').read_text().strip()
    remote = 'C:\\BC250\\m15\\' + attempt
    print('package: run-slot.py Push sends these to ' + remote)
    for f in sorted(pkg.iterdir()):
        print('  %s %7d %s' % (digest(f)[:16], f.stat().st_size, f.name))
    print('lab side (run.py through ssh, run-slot.py):')
    for mode in ('Push', 'Prepare', 'Start'):
        print('  powershell -NoProfile -ExecutionPolicy Bypass -File %s\\dispatch.ps1 -Mode %s -ManifestSha256 %s'
              % (remote, mode, manifest))
    print('config.json:')
    print((pkg / 'config.json').read_text().rstrip())
    tmp = out / 'tmp'
    tmp.mkdir(exist_ok=True)
    print('game-runtime.ps1 -DryRun -Offline (Windows PowerShell 5.1):')
    done = subprocess.run([str(POWERSHELL), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                           str(pkg / 'game-runtime.ps1'), '-DryRun', '-Offline'],
                          env=dict(os.environ, TEMP=str(tmp), TMP=str(tmp)), capture_output=True, text=True)
    print('\n'.join('  ' + line for line in (done.stdout + done.stderr).strip().splitlines()))
    if done.returncode:
        raise SystemExit('runtime dry run failed (exit %d)' % done.returncode)
    # The listing above holds their hashes; the copies of the triplet and the helpers (about 26 MB) go.
    binaries = [f for f in pkg.iterdir() if f.suffix.lower() in ('.dll', '.exe')]
    size = sum(f.stat().st_size for f in binaries)
    for f in binaries:
        f.unlink()
    print('dry package %s kept without its %d binaries (%.1f MB)' % (pkg, len(binaries), size / 1e6))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='command', required=True)
    f = sub.add_parser('facts')
    f.add_argument('profile')
    d = sub.add_parser('dry')
    d.add_argument('n')
    d.add_argument('profile')
    d.add_argument('--out', required=True)
    d.add_argument('--seconds', type=int)
    d.add_argument('--launch', choices=('steam', 'direct'), default='steam')
    d.add_argument('--experiment', default='')
    for q in (f, d):
        q.add_argument('--api', choices=('d3d12', 'd3d11'))
        q.add_argument('--kit', help='a kit directory name under scratch/m15 (default native-caps001)')
    args = p.parse_args()
    facts(args) if args.command == 'facts' else dry(args)


if __name__ == '__main__':
    main()
