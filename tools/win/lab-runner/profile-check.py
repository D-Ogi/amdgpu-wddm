"""Checks every game profile (or the named ones) with run.py's own loader (load_game_profile) and prints what a
session would use: app id, APIs and their router rule and witness, the Steam command line, the executable path, the
process names and the settings policy. Offline: no lab, no Steam data, no game files are read.

    python tools/win/lab-runner/profile-check.py [--run PATH_TO_run.py] [--profiles DIR] [id ...]

--run names another run.py (a work copy before installation); its ROOT is pinned to this workspace.
--profiles names another profiles directory; the default is the one next to this script.
BC250_ROOT names the workspace root (by default the parent directory of this repository)."""
import argparse
import importlib.util
import os
import sys
from pathlib import Path

# bc250-win/tools/win/lab-runner/profile-check.py: the repository root is three levels up.
REPO = Path(__file__).resolve().parents[3]
ROOT = Path(os.environ.get('BC250_ROOT') or REPO.parent)
PROFILES = Path(__file__).resolve().parent / 'profiles'


def load_run(path, profiles):
    # run.py imports the Drive planner from its ROOT at import time; a work copy elsewhere finds it through sys.path.
    sys.path.insert(0, str(REPO / 'tools/win/d3d12queue'))
    spec = importlib.util.spec_from_file_location('run_for_profile_check', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.ROOT = ROOT
    module.GAME_PROFILES = profiles
    return module


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--run', type=Path, default=ROOT / 'scratch/m15/native-caps001/run.py')
    p.add_argument('--profiles', type=Path, default=PROFILES)
    p.add_argument('ids', nargs='*')
    a = p.parse_args()
    profiles = a.profiles.resolve()
    run = load_run(a.run.resolve(), profiles)
    ids = a.ids or sorted(f.stem for f in profiles.glob('*.json'))
    failed = 0
    for name in ids:
        try:
            path, g = run.load_game_profile(name)
        except (RuntimeError, ValueError) as error:
            print(f'FAIL {name}: {error}')
            failed += 1
            continue
        s, e, pr = g['steam'], g['executable'], g['processes']
        exe = '\\'.join(x for x in (s['library'], 'common', s['install_dir'], e['directory'], e['image']) if x)
        print(f"PASS {g['id']}: app {g['app_id']}, api {g['api']} of {','.join(g['apis'])}, readiness {g['readiness']['mode']}, "
              f"settings {g['settings']['policy']}")
        for key, api in g['apis'].items():
            args = ' '.join(x for x in (s['arguments'], api['arguments']) if x)
            print(f"  {key}: steam.exe -applaunch {g['app_id']}{' ' + args if args else ''} | router {api['router']}"
                  f"{' ' + ','.join(g.get('router_images', [])) if api['router'] != 'unchecked' else ''}"
                  f" | witness {api['witness']['module']} {api['witness']['sha256'][:8]}"
                  + ''.join(f" | registry {n['name']}={n['value']}" for n in api.get('registry', [])))
        print(f"  exe {exe}")
        print(f"  main {pr['main']}; chain {','.join(pr['launch_chain'])}; renderers {','.join(pr['renderers'])}; "
              f"survivors {','.join(pr['survivors'])}")
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
