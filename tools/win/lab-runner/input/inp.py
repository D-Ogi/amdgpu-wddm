"""Host side of the game input channel for Steam game sessions outside the native-caps kit (input-server.ps1 in the
lab user's session, started by start-input.ps1). A command goes through the held channel when one serves the session
(channel.py, BD-108: no new ssh connection per command), else through one ssh connection to the input-exec.ps1 that
`start` staged; optional screenshot afterwards through mon.py (0.33 scale, overlay hidden unless --overlay).

    python tools/win/lab-runner/input/inp.py start game-rottr [SECONDS]      start the server (<= 1200 s)
    python inp.py attach game-rottr [SECONDS]     hold one ssh session for the commands (run_in_background, once
                                                  the game runs; it ends at SECONDS, at most 1230, or on detach)
    python inp.py game-rottr "clickat:0.64:0.89;wait:800;tap:1C" [--shot] [--overlay]
    python inp.py game-rottr shot                 screenshot only
    python inp.py game-rottr peek                 queue and server log tail
    python inp.py detach game-rottr               end the held session
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
# Screenshots of the lab screen stay outside the repository (resized, never full resolution).
SHOTS = Path(os.environ.get('BC250_SHOTS_DIR') or ROOT / 'scratch/m15/game-runner/shots')


STAGE = 'C:\\BC250\\tmp\\input'
sys.path.insert(0, str(HERE))
import channel  # noqa: E402


def ps(script, *args, timeout=60):
    r = subprocess.run([sys.executable, str(TARGET), 'ps', str(HERE / script), *args], cwd=str(WIN),
                       capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
    return (r.stdout + r.stderr).strip()


def push():
    # input-exec.ps1 goes with the server, so that each later command is one ssh connection, not three (BD-108).
    subprocess.run([sys.executable, str(TARGET), 'push', str(HERE / 'input-server.ps1'), str(HERE / 'input-exec.ps1'),
                    '--to', STAGE], cwd=str(WIN), capture_output=True, text=True, timeout=90)


def target():
    sys.path.insert(0, str(WIN))
    import target as tgt
    return tgt.Target()


def exec_params(session, peek=False, actions=''):
    if peek:
        return {'Session': session, 'Peek': True}
    return {'Session': session, 'ActionsB64': base64.b64encode(actions.encode('utf-8')).decode('ascii')}


def send(session, params, lab=None):
    """One input-exec.ps1 call: the held channel when it serves the session, else one ssh connection to the staged
    copy. A copy that is not staged (a server started by an older inp.py) is pushed once and the call made again: a
    missing file ran nothing, so that repeat is safe."""
    rep = channel.call(session, 'input-exec.ps1', params)
    if rep is not None:
        return '\n'.join(rep['lines'])
    lab = lab or target()
    args = []
    for key, value in params.items():
        args += [f'-{key}'] if value is True else [f'-{key}', str(value)]
    remote = STAGE + '\\input-exec.ps1'
    done = lab.run_staged(remote, args, timeout=60)
    text = (done.stdout + done.stderr).strip()
    if done.returncode and 'input-exec.ps1' in text and ('does not exist' in text or 'not recognized' in text):
        lab.push([str(HERE / 'input-exec.ps1')], STAGE)
        done = lab.run_staged(remote, args, timeout=60)
        text = (done.stdout + done.stderr).strip()
    return text


def shot(session, overlay=False):
    folder = SHOTS / session
    folder.mkdir(parents=True, exist_ok=True)
    out = folder / time.strftime('shot-%H%M%S.png', time.gmtime())
    # A failed shot is an alarm, not a quiet line: the lab may have crashed (RotTR 0x116, 2026-10-01, seen first
    # on the lab monitor while this printed "shot" for files that never came).
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
        channel.main(['stop', a[1]])
        print(ps('start-input.ps1', '-Session', a[1], '-Stop'))
        return
    if a[0] == 'attach':
        return channel.main(['serve', a[1], a[2] if len(a) > 2 else '900', str(HERE / 'input-exec.ps1')])
    if a[0] == 'detach':
        return channel.main(['stop', a[1]])
    session, actions = a[0], a[1]
    flags = set(a[2:])
    if actions == 'shot':
        shot(session, '--overlay' in flags)
        return
    if actions == 'peek':
        print(send(session, exec_params(session, peek=True)))
        return
    print(send(session, exec_params(session, actions=actions)))
    if '--shot' in flags:
        shot(session, '--overlay' in flags)


if __name__ == '__main__':
    sys.exit(main() or 0)
