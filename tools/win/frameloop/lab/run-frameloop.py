#!/usr/bin/env python3
"""One bounded set of amdgpu_wddm_frameloop runs on unit A, from here.

    python lab\\run-frameloop.py quick|mt|gap [--trial NAME] [--dry-run]

What it does, in one SSH call each: the owner's STOP flag, a line on the overlay, the client and a plan file
pushed to C:\\BC250\\frameloop, lab\\frameloop-task.ps1 run there (it starts every configuration as a one-shot
scheduled task in the console session, because an SSH session is session 0 and can show no window), the result
archive pulled back into lab\\runs\\<trial>, and one summary row per configuration.

The sets are in lab\\sets.json. Nothing here swaps a driver file: the client goes through the registered
triplet, which is what the measurement is about. --dry-run prints the plan and the commands and touches
nothing. The lab bound is three minutes for the whole set (CLAUDE.md); the runner refuses a set that would
not fit and the lab side skips what would run past it.
"""
import json
import os
import re
import subprocess
import sys
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FRAMELOOP = HERE.parent
TOOLS_WIN = FRAMELOOP.parent
# BC250_ROOT is the workspace root; otherwise the first directory above this one that holds the toolchain,
# which is what src\build.ps1 and src\work-dir.ps1 do. A fixed number of parents is wrong in a worktree,
# where the repository is not a child of the workspace root.
def _workspace_root():
    root = os.environ.get("BC250_ROOT")
    if root:
        return Path(root)
    for candidate in FRAMELOOP.parents:
        if (candidate / "toolchain").is_dir():
            return candidate
    return FRAMELOOP.parents[3]


ROOT = _workspace_root()
# The client binary and the evidence of a trial stay in the workspace, never in the repository
# (BC250_FRAMELOOP_WORK overrides the place): <work>/build holds the built client, <work>/lab the plan file
# of every trial and <work>/lab/runs/<trial> what was pulled back from the lab.
WORK = Path(os.environ.get("BC250_FRAMELOOP_WORK") or (ROOT / "scratch" / "m15" / "frameloop"))
LAB_WORK = WORK / "lab"
REMOTE_DIR = "C:\\BC250\\frameloop"
# The lab's bound for one trial is three minutes (CLAUDE.md), and the set must fit in it with the archive.
#
# What a run costs besides its own --seconds: registering and starting the task, the client's own startup
# (runtime, adapter, device, window, swap chain, the GPU-work calibration), one second of poll granularity,
# unregistering, and one second of settling. The client now writes that startup as a `timeline` block, so it is
# measured, not guessed: on this machine it is 0.8 s without the override and 1.9 s with it (the sustained
# calibration pass runs again), the rest is about 4 s, so 8 s is the planned overhead per run. The lab's own
# number arrives with the next trial and this constant is revised from it.
RUN_OVERHEAD_SECONDS = 8
# The worst case, used by the lab side to decide whether the next run still fits: the client's watchdog asks the
# loop to stop at seconds + 15 and kills at seconds + 20, the task limit is seconds + 25 and the poll gives up at
# seconds + 30. A run that goes wrong therefore ends at seconds + 30, never later, and the set stays inside its
# deadline even if every run goes wrong.
RUN_WORST_CASE_EXTRA = 30
SET_BUDGET_SECONDS = 170

sys.path.insert(0, str(TOOLS_WIN))


def fail(message):
    raise SystemExit("run-frameloop: " + message)


def load_set(name):
    data = json.loads((HERE / "sets.json").read_text(encoding="utf-8"))
    if name not in data["sets"]:
        fail("no set %r; sets.json has %s" % (name, ", ".join(sorted(data["sets"]))))
    chosen = data["sets"][name]
    common = [str(a) for a in data.get("common", [])]
    runs = []
    for run in chosen["runs"]:
        seconds = int(run["seconds"])
        if not 1 <= seconds <= 60:
            fail("run %s asks for %d s; the client takes 1..60" % (run["name"], seconds))
        # Common options first, the run's own after them: the client keeps the last value it is given, so a run
        # that needs three buffers can say so without the set having to drop the default.
        args = common + ["--seconds", str(seconds)] + [str(a) for a in run["args"]]
        # Driver knobs of this run alone. Only BC250_* names, checked here and again on the lab, so a set can
        # hold both arms of a knob (BC250_SELF_WAIT keep against elide) without a machine-wide cfg file that
        # would move the desktop's own ICD at the same time.
        env = {}
        for key, value in (run.get("env") or {}).items():
            if not re.fullmatch(r"BC250_[A-Z0-9_]+", str(key)) or re.search(r'["%\r\n]', str(value)):
                fail("run %s: %r=%r is not a usable BC250 knob" % (run["name"], key, value))
            env[str(key)] = str(value)
        runs.append({"name": run["name"], "seconds": seconds, "argline": " ".join(args), "env": env})
    budget = sum(r["seconds"] + RUN_OVERHEAD_SECONDS for r in runs)
    if budget > SET_BUDGET_SECONDS:
        fail("set %s needs about %d s, over the %d s bound: shorten it or split it"
             % (name, budget, SET_BUDGET_SECONDS))
    # The planned budget is an expectation; the deadline is the lab bound. A run that hits its worst case must
    # still leave room for the next one to be skipped rather than overrun, so the longest run plus its worst case
    # has to fit in the deadline on its own.
    worst = max(r["seconds"] for r in runs) + RUN_WORST_CASE_EXTRA
    if worst > SET_BUDGET_SECONDS:
        fail("run of %d s could take %d s in the worst case, over the %d s bound"
             % (max(r["seconds"] for r in runs), worst, SET_BUDGET_SECONDS))
    return chosen, runs, budget


def sha256(path):
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


def monitor(args, check=False):
    """One mon.py call. The overlay is how the owner sees what the lab is about to do; a monitor that is not
    answering must not stop a trial, so only the STOP gate is checked."""
    done = subprocess.run([sys.executable, str(TOOLS_WIN / "bc250mon" / "mon.py")]
                          + [str(a) for a in args], cwd=str(ROOT), capture_output=True, text=True,
                          errors="replace")
    if check and done.returncode:
        fail("mon.py %s failed: %s" % (args[0], (done.stdout + done.stderr).strip()[:200]))
    return done


def summarize(directory):
    index_path = directory / "index.json"
    if index_path.exists():
        index = json.loads(index_path.read_text(encoding="utf-8-sig"))
        print("lab: trial %s, set %s, %s s, console session %s, driver %s, Tctl %s -> %s C"
              % (index.get("trial"), index.get("set"), index.get("elapsed_seconds"),
                 index.get("console_session"), index.get("kmd_version") or "?",
                 index.get("tctl_before_c"), index.get("tctl_after_c")))
        # The operating point is not a detail of the run, it is part of what the numbers mean: since DPM
        # shipped the clock is the governor's choice, and K178 cost three days of game numbers that were
        # silently 24 CU.
        cu = index.get("cu_record") or {}
        print("lab: CU record mode %s, confirmed %s, last applied %s, disable_wgp %s (registry, %s);"
              " clock reader %s"
              % (cu.get("CuMode"), cu.get("CuModeConfirmed"), cu.get("CuModeLastApplied"),
                 cu.get("CuDisableWgp"),
                 "present" if cu.get("parameters_present") else "no Parameters key",
                 index.get("clock_reader") or "?"))
        # The plan's order, not the alphabet: the rows are meant to be read against each other.
        states = [(r["name"], r) for r in index.get("runs", [])]
    else:
        print("lab: no index.json in the archive")
        states = [(p.stem, {}) for p in sorted(directory.glob("*.json"))]
    header = ("run", "fps", "intrvl", "busy", "idle", "await", "after", "sub2st", "wake", "rec", "recall",
              "exec", "pres", "ratio", "err%", "exit")
    print("%-20s %7s %7s %7s %7s %7s %7s %7s %7s %7s %7s %7s %7s %7s %7s %5s" % header)
    for name, state in states:
        path = directory / (name + ".json")
        if state.get("skipped"):
            print("%-20s SKIPPED %s" % (name, state["skipped"]))
            continue
        if not path.exists():
            print("%-20s no JSON (task %s, exit %s); see %s.txt"
                  % (name, state.get("task_state"), state.get("exit_code"), name))
            continue
        data = json.loads(path.read_text(encoding="utf-8"))
        s, c = data["summary"], data["calibration"]
        gpu, cpu, lat = s["gpu_ms"], s["cpu_ms"], s["latency_ms"]
        consistency = s.get("consistency", {})

        def p50(block, key):
            return block[key]["p50"] if key in block else float("nan")

        print("%-20s %7.1f %7.3f %7.3f %7.3f %7.3f %7.3f %7.2f %7.3f %7.3f %7.3f %7.3f %7.3f %7.4f %+7.1f %5s"
              % (name, s["fps"], p50(cpu, "frame_interval"), p50(gpu, "busy"), p50(gpu, "idle_per_frame"),
                 p50(gpu, "idle_awaiting_submission_per_frame"), p50(gpu, "idle_after_submission_per_frame"),
                 p50(lat, "submit_to_gpu_start"), p50(lat, "gpu_end_to_wake_blocking"), p50(cpu, "record"),
                 p50(cpu, "record_cpu_total"), p50(cpu, "execute_total"), p50(cpu, "present"),
                 consistency.get("gpu_over_interval", float("nan")), c["gpu_busy_error_pct"],
                 state.get("exit_code")))
        notes = []

        def point(which):
            block = state.get("point_" + which) or {}
            clock = block.get("clock") or {}
            if "mhz" in clock:
                return "%s MHz VID %s, %s C" % (clock.get("mhz"), clock.get("vid"), block.get("tctl_c"))
            return "%s (Tctl %s C)" % (clock.get("error") or "no clock reader", block.get("tctl_c"))

        if state.get("point_before") or state.get("point_after"):
            notes.append("operating point %s -> %s" % (point("before"), point("after")))
        # Where the wall time went, so the budgets can be sized from runs instead of guesses.
        phases = {p["phase"]: p["ms"] for p in data.get("timeline", [])}
        if phases:
            loop_ms = phases.get("loop", 0.0)
            total = sum(phases.values())
            notes.append("wall %.1f s = %.1f s outside the loop (device %.0f, pipeline %.0f, calibration %.0f,"
                         " fit %.0f, teardown %.0f ms)"
                         % (total / 1000.0, (total - loop_ms) / 1000.0, phases.get("device", 0.0),
                            phases.get("pipeline", 0.0),
                            phases.get("calibration", 0.0) + phases.get("sustained calibration", 0.0),
                            phases.get("clock fit", 0.0), phases.get("teardown", 0.0)))
        if data["config"].get("ts_hz"):
            notes.append("ts-hz override %d Hz against the %s Hz the driver reports (BD-056): the GPU columns are"
                         " milliseconds on the real counter"
                         % (data["config"]["ts_hz"], data["host"].get("gpu_timestamp_frequency_reported", "?")))
        fit = data.get("clock_fit", {})
        if fit.get("applied"):
            notes.append("clock origin fitted (bracket %.3f ms, %d refits, %d clamped): the GPU-side durations"
                         " are exact, the latencies and the await/after split carry that uncertainty"
                         % (fit.get("bracket_ms", 0.0), fit.get("refits", 0), fit.get("refits_clamped", 0)))
        if not consistency.get("cross_clock_usable", True):
            notes.append("CROSS-CLOCK NOT USABLE: read busy, gaps, idle and the cpu_ms columns, not sub2st/wake")
        if not consistency.get("timestamp_scale_ok", True):
            notes.append("TIMESTAMP SCALE %.4f: the GPU-side columns are not milliseconds"
                         % consistency.get("gpu_over_interval", 0.0))
        if s["invalid_timestamp_frames"]:
            notes.append("%d frames with invalid timestamps" % s["invalid_timestamp_frames"])
        if data["result"]["status"] != "PASS":
            notes.append("status %s at %s" % (data["result"]["status"], data["result"]["failure_where"]))
        for note in notes:
            print("%-20s   %s" % ("", note))
    print("columns: p50 ms unless named; await/after = the idle before and after the work was submitted;"
          " ratio = GPU timeline over frame interval (1.00 = the timestamps are on the reported clock)")
    witness(directory, [name for name, _ in states])


def icd_counters(path):
    """The knobs and counters of the last submit line of an ICD log, as the client's icd block names them."""
    line = ""
    for text in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if " submit: " in text:
            line = text
    fields = dict(kv.split("=", 1) for kv in line.split() if "=" in kv)
    out = {"submit_line": line, "self_wait": fields.get("self_wait", ""), "coalesce": fields.get("coalesce", "")}
    for key in ("wait_self_seen", "wait_self_dropped", "wait_self_busy", "wait_objects", "wait_dropped",
                "wait_map_dropped", "wait_calls"):
        value = fields.get(key, "")
        out[key] = int(value) if value.isdigit() else None
    return out


def witness(directory, names):
    """What the arms prove about themselves, before anything is compared between them.

    An arm of BC250_SELF_WAIT is void unless the ICD's own counters say it took effect, and the
    vblank-aligned share means nothing unless the grid fitted and the clock bracket is inside the phase
    window. Session 322 is why this is a gate and not a note: there, the mechanism counters moved, the
    kernel wait calls fell to 17, and the frame rate fell 1.7 % (K142). A mechanism witness is not a
    result, and a comparison printed next to a failed witness gets quoted as one."""
    refusals, lines = [], []
    for name in names:
        path = directory / (name + ".json")
        if not path.exists():
            continue
        # utf-8-sig: Windows PowerShell 5.1 writes a BOM with -Encoding UTF8, and the host checks write with it.
        data = json.loads(path.read_text(encoding="utf-8-sig"))
        # The gaps block is under summary since schema 4. Reading it at the top level found nothing, which
        # read as "no vblank grid" for both C45 sets of 2026-10-05 although every arm had fitted one.
        icd = data.get("icd")
        gaps = (data.get("summary") or {}).get("gaps") or data.get("gaps") or {}
        pulled = directory / (name + "-icd.log")
        if icd is not None and not icd.get("submit_line") and pulled.exists():
            # A client before the _fsopen fix could not read the log the ICD still held; the archive carries the
            # same log, pulled after the process ended. Its last submit line is cumulative, so the drop and seen
            # counters witness the arm the same way (they include the warm-up, which only adds to both).
            icd = dict(icd, read="ok (pulled log)", **icd_counters(pulled))
        arm = (icd or {}).get("self_wait")
        if icd is None:
            refusals.append("%s: no ICD log, so nothing says which winsys ran this client" % name)
        elif not str(icd.get("read", "ok")).startswith("ok") or not icd.get("submit_line"):
            refusals.append("%s: the ICD log was not read back (%s): no counter says which arm ran"
                            % (name, icd.get("read", "no submit line")))
        else:
            seen, dropped = icd.get("wait_self_seen"), icd.get("wait_self_dropped")
            busy = icd.get("wait_self_busy")
            lines.append("%-20s arm %-6s seen %s dropped %s busy %s"
                         % (name, arm, seen, dropped, busy))
            if arm == "elide" and not dropped:
                refusals.append("%s: arm elide dropped no wait at all; the client did not run through the"
                                " candidate ICD, or the record never matched" % name)
            if arm == "count" and (not seen or dropped):
                refusals.append("%s: arm count must see matches and drop none (seen %s, dropped %s)"
                                % (name, seen, dropped))
            if arm in ("keep", None) and (seen or dropped):
                refusals.append("%s: arm %s moved the self-wait counters (seen %s, dropped %s)"
                                % (name, arm, seen, dropped))
            if busy:
                refusals.append("%s: %s submissions found the self-signal record in use; one queue's record"
                                " had more than one user, so the arm is not what it says" % (name, busy))
        # One failed call is the first one after the swap chain starts (no statistics yet); the grid is fitted
        # from the rest. More than 1 % failed means the grid is fitted from a different display state.
        failed, calls = gaps.get("frame_statistics_failures") or 0, gaps.get("frame_statistics_calls") or 0
        if failed > max(1, calls // 100):
            refusals.append("%s: %s of %s GetFrameStatistics calls failed" % (name, failed, calls))
        # An arm that was never given --frame-statistics has no vblank grid by design (the smoke run of every
        # set is one), and refusing it withheld the comparison of every trial this gate has ever read - which
        # made the refusal list say nothing about the arms that matter. Only an arm that asked for the grid and
        # did not get one is a refusal.
        wants_grid = bool((data.get("config") or {}).get("frame_statistics"))
        if not wants_grid:
            lines.append("%-20s no --frame-statistics: no vblank grid asked for, aligned shares are null"
                         % name)
        elif not ((gaps.get("vblank") or {}).get("fitted")):
            refusals.append("%s: no vblank grid, so every aligned share is null" % name)
        elif not gaps.get("aligned_share_usable", True):
            refusals.append("%s: clock bracket %s ms against a %s us phase window: the aligned shares are"
                            " not reported" % (name, gaps.get("phase_bracket_ms"), gaps.get("phase_window_us")))
        # The cross-queue arms witness themselves the same way: a handoff whose fence never moved did not run,
        # and a control whose fence did move is not a control. The thresholds are the measured ratios of the
        # host matrix (2026-10-06, 7 lists): 6.7 waits a frame for per-list, 1.1 for per-frame, 7.8 signals.
        # The full rule for these arms lives in scratch\m15\etw\c48\c48repro.py; this is only the refusal.
        handoff = data.get("queue_handoff")
        if handoff:
            mode = handoff.get("handoff", "none")
            signals, waits = handoff.get("fence_signals", 0), handoff.get("fence_waits", 0)
            frames = (data.get("summary") or {}).get("frames_measured") or 0
            signalling = mode != "none" or handoff.get("signal_per_list")
            # The clock witness is part of the arm saying what it is: a second queue whose timestamp domain
            # does not agree with the first makes every cross-queue duration in the trial meaningless, and the
            # client reports which of the three disagreements it saw.
            clock = handoff.get("queue_clock_witness") or "not reported"
            lines.append("%-20s queues %s handoff %-9s signals %s waits %s clock %s"
                         % (name, handoff.get("queues_created"), mode, signals, waits, clock))
            if (handoff.get("queues_created") or 1) > 1 and clock != "one domain":
                refusals.append("%s: the second queue's clock witness says '%s', so the two queues do not"
                                " share a timestamp domain" % (name, clock))
            if signalling and signals < frames:
                refusals.append("%s: %s fence signals for %s frames: the %s arm did not run"
                                % (name, signals, frames, mode if mode != "none" else "signal-per-list"))
            if not signalling and (signals or waits):
                refusals.append("%s: a control arm moved the handoff fence (%s signals, %s waits)"
                                % (name, signals, waits))
            if mode == "per-list" and waits < frames:
                refusals.append("%s: %s fence waits for %s frames: the per-list handoff did not cross"
                                % (name, waits, frames))
            if mode == "per-frame" and waits < frames // 2:
                refusals.append("%s: %s fence waits for %s frames: the per-frame handoff did not cross"
                                % (name, waits, frames))
    for line in lines:
        print("witness: " + line)
    if refusals:
        print("COMPARISON WITHHELD, the arms do not witness themselves:")
        for refusal in refusals:
            print("  - " + refusal)
        print("  Fix the arm and run again. Do not quote a rate or a share from this trial (K142: a counter"
              " of avoided kernel calls is not time).")
    elif lines:
        print("witness: every arm says what it is; the comparison may be read (decision rule in sets.json)")


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        raise SystemExit(__doc__)
    set_name = argv[0]
    rest = argv[1:]
    dry = "--dry-run" in rest
    trial = None
    if "--trial" in rest:
        trial = rest[rest.index("--trial") + 1]
    trial = trial or (set_name + "-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()))
    chosen, runs, budget = load_set(set_name)
    exe = WORK / "build" / "amdgpu_wddm_frameloop.exe"
    if not exe.exists():
        fail("no client at %s; build it with src\\build.ps1" % exe)
    plan = {"trial": trial, "set": set_name, "dir": REMOTE_DIR, "exe_sha256": sha256(exe),
            "budget_seconds": budget, "deadline_seconds": SET_BUDGET_SECONDS,
            "run_overhead_seconds": RUN_OVERHEAD_SECONDS,
            "run_worst_case_extra_seconds": RUN_WORST_CASE_EXTRA, "temp_cap_c": 87.0, "runs": runs}
    plan_path = LAB_WORK / ("plan-%s.json" % trial)
    # Only a real trial leaves a plan file behind: a dry run that wrote one would leave a record of a trial that
    # never happened next to the records of the ones that did.
    if not dry:
        plan_path.parent.mkdir(parents=True, exist_ok=True)
        plan_path.write_text(json.dumps(plan, indent=1) + "\n", encoding="utf-8")
    print("set %s: %s" % (set_name, chosen["what"]))
    print("trial %s, %d runs, about %d s (deadline %d s), client %s"
          % (trial, len(runs), budget, SET_BUDGET_SECONDS, plan["exe_sha256"][:8]))
    for run in runs:
        print("  %-20s %s" % (run["name"], run["argline"]))
        if run["env"]:
            print("  %-20s knobs %s" % ("", " ".join("%s=%s" % kv for kv in sorted(run["env"].items()))))
    # An override of the timestamp clock changes what every GPU-side number means; it must never be invisible in
    # the record of a trial.
    if any("--ts-hz" in run["argline"] for run in runs):
        hz = runs[0]["argline"].split("--ts-hz")[1].split()[0]
        print("NOTE: --ts-hz %s overrides the clock the driver reports (BD-056, interim until KMD 0.7.197)." % hz)
        print("      GPU-side durations are then real milliseconds and --gpu-ms calibrates against this clock;")
        print("      the clock origin is fitted, so the latency columns carry the fit's bracket. Drop the")
        print("      override once the KMD answers CalibrateGpuClock from GOLDEN_TSC.")
    if dry:
        print("dry run: nothing was sent. The calls would be:")
        print("  mon.py stop? ; mon.py status ... ; mon.py log ...")
        print("  target.py push %s %s --to %s" % (exe, plan_path, REMOTE_DIR))
        print("  target.py ps lab\\frameloop-task.ps1 -Plan %s\\%s   (one call, the whole set)"
              % (REMOTE_DIR, plan_path.name))
        print("  target.py pull %s\\runs-%s.zip -> %s\\runs\\%s\\" % (REMOTE_DIR, trial, LAB_WORK, trial))
        return 0

    import target  # only now: importing it is free, but a dry run should not even read the lab configuration

    stop = monitor(["stop?"])
    if "no stop request" not in stop.stdout:
        fail("the owner asked to stop: " + (stop.stdout + stop.stderr).strip()[:200])
    monitor(["status", "frameloop %s: %d runs, about %d s, the screen will be covered"
             % (set_name, len(runs), budget), "info"])
    monitor(["log", "frameloop trial %s, client %s" % (trial, plan["exe_sha256"][:8])])

    lab = target.Target()
    print("target %s" % lab.address)
    # The exe, the plan and the wrapper generator in one tar; run_script copies the task script itself on its
    # way to running it, and that script dot-sources cmd-lines.ps1 from the same directory.
    lab.push([str(exe), str(plan_path), str(HERE / "cmd-lines.ps1")], REMOTE_DIR)
    remote_plan = REMOTE_DIR + "\\" + plan_path.name
    # The lab side bounds itself at the deadline; this is only the SSH call's own patience, above the deadline plus
    # the archive, so a hung call is still reported here rather than waited on for ever.
    done = lab.run_script(str(HERE / "frameloop-task.ps1"), ["-Plan", remote_plan],
                          timeout=SET_BUDGET_SECONDS + 90, remote_dir=REMOTE_DIR)
    print((done.stdout or "").rstrip())
    if done.stderr.strip():
        print("stderr: " + done.stderr.strip()[:2000])
    monitor(["status", "frameloop %s done" % set_name, "good"])
    if done.returncode:
        fail("the lab side failed (exit %d); nothing was pulled, the evidence is in %s\\runs\\%s"
             % (done.returncode, REMOTE_DIR, trial))

    local = LAB_WORK / "runs" / trial
    local.mkdir(parents=True, exist_ok=True)
    archive = local.parent / ("%s.zip" % trial)
    lab.pull(REMOTE_DIR + "\\runs-%s.zip" % trial, str(archive))
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(local)
    os.remove(archive)
    print("pulled %d files into %s" % (len(list(local.iterdir())), local))
    summarize(local)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
