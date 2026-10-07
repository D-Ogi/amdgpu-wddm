"""C62 grid sweep driver (dev PC). Runs c62-trial.ps1 on the lab once per grid cell, one ssh call per trial, cools
down between trials (fan 100 % lease, Tctl below COOL_C, one probe per 30 s), samples the smart plug every 15 s during
a trial, and writes runs/<tag>.txt (lab output) and runs/<tag>.plug.jsonl. Resumes: a cell with an output is skipped.
Usage: python sweep.py [--only TAG ...] [--seconds 150]"""
import argparse, itertools, json, pathlib, subprocess, sys, threading, time, datetime

HERE = pathlib.Path(__file__).resolve().parent
RUNS = HERE / 'runs'
TARGET = [sys.executable, r'P:\bc-250\bc250-win\tools\win\target.py']
PLUG = [sys.executable, r'P:\bc-250\scratch\smartplug\plug.py', 'telemetry']
COOL_C = 62.0
FANS = [0, 50, 100]          # 0 = the driver's Standard curve
GPU_MV = [0, 25]             # V/F offset off every allowed level
CPU_UV = [0, 4]              # firmware curve-scale steps, about 7 mV each at 3500 MHz (K204)


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat(timespec='seconds')


def lab_ps(script, args=(), timeout=300):
    cmd = TARGET + ['ps', str(script)] + list(args)
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, encoding='utf-8', errors='replace')
    return p.returncode, p.stdout + p.stderr


def cool_down():
    rc, out = lab_ps(HERE / 'c62-cool.ps1', ['-Seconds', '300'], timeout=400)
    print(out.strip().splitlines()[-1] if out.strip() else f'cool rc={rc}', flush=True)


def plug_sampler(path, stop):
    with open(path, 'a', encoding='utf-8') as f:
        while not stop.is_set():
            try:
                p = subprocess.run(PLUG, capture_output=True, text=True, timeout=20)
                f.write(json.dumps({'utc': utc(), 'out': p.stdout.strip()}) + '\n')
                f.flush()
            except Exception as e:
                f.write(json.dumps({'utc': utc(), 'error': str(e)}) + '\n')
            stop.wait(15)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--only', nargs='*')
    ap.add_argument('--seconds', type=int, default=75)
    ap.add_argument('--budget', type=int, default=900, help='seconds; no new cell starts past it')
    a = ap.parse_args()
    RUNS.mkdir(exist_ok=True)
    # Most informative cells first: the owner's time box (about 15 min, 2026-10-07) may end the sweep early, and a
    # later run resumes with the cells left.
    first = [(0, 0, 0), (0, 25, 0), (0, 0, 4), (0, 25, 4), (50, 0, 0), (100, 25, 4)]
    cells = first + [c for c in itertools.product(FANS, GPU_MV, CPU_UV) if c not in first]
    deadline = time.monotonic() + a.budget
    for fan, gmv, cuv in cells:
        if time.monotonic() + a.seconds + 100 > deadline:
            print(f'{utc()} time box: cells left for a later run', flush=True)
            break
        tag = f'f{fan}-g{gmv}-c{cuv}'
        if a.only and tag not in a.only:
            continue
        out = RUNS / f'{tag}.txt'
        if out.exists():
            continue
        cool_down()
        stop = threading.Event()
        th = threading.Thread(target=plug_sampler, args=(RUNS / f'{tag}.plug.jsonl', stop), daemon=True)
        th.start()
        print(f'{utc()} trial {tag}', flush=True)
        rc, text = lab_ps(HERE / 'c62-trial.ps1', ['-FanPct', str(fan), '-GpuMv', str(gmv), '-CpuUv', str(cuv),
                                                   '-Seconds', str(a.seconds), '-Tag', tag], timeout=a.seconds + 120)
        stop.set(); th.join(30)
        out.write_text(f'# {utc()} rc={rc}\n' + text, encoding='utf-8')
        summary = [l for l in text.splitlines() if l.startswith(('stop=', 'WHEA', '4101'))]
        print(f'{utc()} {tag} rc={rc} ' + ' | '.join(summary), flush=True)
        if 'owner STOP' in text:
            print('owner STOP: sweep ends', flush=True)
            return 3
        if any(l.startswith('WHEA') and not l.endswith(': 0') for l in summary) and cuv:
            print('WHEA after an undervolt cell: sweep ends for review', flush=True)
            return 4
    return 0


if __name__ == '__main__':
    sys.exit(main())
