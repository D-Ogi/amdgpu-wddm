"""Host side of the game input channel for Steam game sessions outside the native-caps kit (input-server.ps1 in the
lab user's session, started by start-input.ps1). One ssh call per command; optional screenshot afterwards through
mon.py (0.33 scale, overlay hidden unless --overlay).

    python tools/win/lab-runner/input/inp.py start game-rottr [SECONDS]      start the server (<= 1200 s)
    python inp.py game-rottr "clickat:0.64:0.89;wait:800;tap:1C" [--shot] [--overlay]
    python inp.py game-rottr shot                 screenshot only
    python inp.py game-rottr peek                 queue and server log tail
    python inp.py stop game-rottr                 quit + remove the task

Actions: tap:SCAN tapx:SCAN hold:SCAN:MS holdx:SCAN:MS click:left|right[:MS] clickat:FX:FY[:MS] point:FX:FY
look:DX:DY:N:MS wheel:N wait:MS quit. Scan codes hex: Enter 1C, Esc 01, Space 39, Tab 0F, W 11, A 1E, S 1F, D 20,
E 12, arrows (tapx) up 48 down 50 left 4B right 4D. Screenshots never enter this repository: they go to <BC250_ROOT>\\scratch\\m15\\game-runner\\shots\\<session>\\
(BC250_SHOTS_DIR names another directory outside the repository)."""
import base64
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
# bc250-win/tools/win/lab-runner/input/inp.py: tools/win is two levels up, the repository four.
WIN = HERE.parents[1]
REPO = HERE.parents[3]
ROOT = Path(os.environ.get('BC250_ROOT') or REPO.parent)
TARGET = WIN / 'target.py'
MON = WIN / 'bc250mon/mon.py'
# Screenshots of the lab screen stay outside the repository (owner: resized, never full resolution).
SHOTS = Path(os.environ.get('BC250_SHOTS_DIR') or ROOT / 'scratch/m15/game-runner/shots')


def ps(script, *args, timeout=60):
    r = subprocess.run([sys.executable, str(TARGET), 'ps', str(HERE / script), *args], cwd=str(WIN),
                       capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
    return (r.stdout + r.stderr).strip()


def push():
    subprocess.run([sys.executable, str(TARGET), 'push', str(HERE / 'input-server.ps1'), '--to', 'C:\\BC250\\tmp\\input'],
                   cwd=str(WIN), capture_output=True, text=True, timeout=90)


def shot(session, overlay=False):
    folder = SHOTS / session
    folder.mkdir(parents=True, exist_ok=True)
    out = folder / time.strftime('shot-%H%M%S.png', time.gmtime())
    # A failed shot is an alarm, not a quiet line: the lab may have crashed (RotTR 0x116, 2026-10-01, seen first
    # by the owner while this printed "shot" for files that never came).
    try:
        r = subprocess.run([sys.executable, str(MON), 'screenshot', '--scale', '0.33', '--overlay',
                            '1' if overlay else '0', '--out', str(out)], cwd=str(WIN), capture_output=True,
                           text=True, encoding='utf-8', errors='replace', timeout=60)
        failed = r.returncode != 0 or not out.exists() or out.stat().st_size == 0
        detail = (r.stdout + r.stderr).strip()[-300:]
    except subprocess.TimeoutExpired:
        failed, detail = True, 'timeout 60 s'
    if failed:
        print(f'ALARM: shot FAILED ({detail}); check the lab (boot-state.ps1, plug, camera) before anything else')
        sys.exit(3)
    print(f'shot {out}')


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        raise SystemExit(__doc__)
    if a[0] == 'start':
        push()
        print(ps('start-input.ps1', '-Session', a[1], '-Seconds', a[2] if len(a) > 2 else '900'))
        return
    if a[0] == 'stop':
        print(ps('start-input.ps1', '-Session', a[1], '-Stop'))
        return
    session, actions = a[0], a[1]
    flags = set(a[2:])
    if actions == 'shot':
        shot(session, '--overlay' in flags)
        return
    if actions == 'peek':
        print(ps('input-exec.ps1', '-Session', session, '-Peek'))
        return
    encoded = base64.b64encode(actions.encode('utf-8')).decode('ascii')
    print(ps('input-exec.ps1', '-Session', session, '-ActionsB64', encoded))
    if '--shot' in flags:
        shot(session, '--overlay' in flags)


if __name__ == '__main__':
    main()
