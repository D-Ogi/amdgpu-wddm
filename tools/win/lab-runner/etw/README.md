# lab-runner/etw - the ETW capture inside a game trial

A game trial on the lab holds the GPU, the screen and a three-digit trial directory, and it ends at a deadline
that the trial itself owns. This is the instrument that records what happened inside such a trial: a bounded
ETW capture of the GPU scheduler and, when it is asked for, a full CPU profile of the game and of DWM.

The capture runs on the lab as a one-shot SYSTEM task. The operator never waits with an open SSH session. One
call starts it, one call closes it, and the closure is an independent receipt rather than a claim of the task
that produced the data.

```
python tools/win/target.py push tools\win\lab-runner\etw\etw-capture.ps1 --to C:\BC250\tmp
python tools/win/target.py ps tools\win\lab-runner\etw\etw-start.ps1 -Trial 416 -Seconds 30 [-FpsSeconds 60]
                                                                 [-WorldSeconds 40] [-PresentMode]
python tools/win/target.py ps tools\win\lab-runner\etw\etw-closure.ps1 -Trial 416
pwsh -NoProfile -File tools\win\lab-runner\etw\host-checks.ps1      host: the shape of all three, no lab
```

`etw-start.ps1` stages the capture and registers the task. `etw-capture.ps1` is the instrument itself and runs
on the lab. `etw-closure.ps1` is the operator's receipt and runs once the task is no longer `Running`.

## The contract

1. **Two windows, A and B.** Window A starts `-StartA` seconds after the game appears and lasts `-Seconds`,
   because DWM misbehaves from the start of a game. Window B starts when dwm.exe takes more than `-DwmPct` per
   cent of the machine in two consecutive 2 s samples, or at `-LatestB` seconds after the game appeared,
   whichever comes first. `-SkipA` drops window A. `-WorldLog` starts window B from the game's own log, when
   the world is up.
2. **One deadline bounds everything.** `-NotAfterQpc` is the trial's own runtime cutoff on the shared QPC
   clock. `etw-start.ps1` reads the trial's `start.json` and computes it, so operator delay before the call
   cannot extend the capture. The capture starts a window only when the collection plus the cleanup reserve
   fits before that deadline, and it cuts every wait to what is left.
3. **`-GpuOnly` for a frame-rate run.** It records the GPU scheduler alone, with no CPU profile, so the
   measurement does not load the CPU. The ETW observer costs about 1.6 ms per frame on the game's main thread
   with the thread-time profile on, which is a measurable part of a frame rate.
4. **Cleanup runs in `finally`.** Each window owns the sessions named `BC250-<Tag>-<W>*`. The capture stops
   what it owns, and a failed session query never counts as absence.
5. **The task exits 1 when a cleanup failed.** An ETW session that outlives its task keeps writing to the
   lab's disk.
6. **The closure receipt is independent.** `etw-closure.ps1` first checks that the producer is no longer
   `Running` and that no helper of this trial is alive. Only then it stops the owned sessions, takes the final
   query, and removes the task. Exit 0 closure, 1 sessions remain, 2 a query failed, 3 the producer runs,
   4 a helper could not be joined. Between two windows zero sessions can be listed while the task is about to
   start the next one, so the task state comes first.

`-PresentMode` adds Win32k and Dwm-Core to the GPU session with narrow keyword masks, so that a per-frame
present classifier can say whether a present was scanned out or composed (M15.14). It changes nothing else
about a window and raises the session sizing from 20 to 28 MB per second.

`-SchedulerStacks` answers a different question from everything above: not what a frame costs, but which
dxgkrnl call site lets a node run the packet it is already holding. In sessions 418-420 the 3D ring stood idle
for a median 8 ms with a dispatchable packet queued, 418-605 times per 105 s, and the gap ended within 300 us
of a display VSync. The packet-level session can see that; it cannot see who lifted the gate. With this switch
the window records, in PerfView's own session, the scheduler's decisions with a call stack on each
(`UpdateContextStatus` id 20, `UnwaitQueuePacket` 238, `SelectContext2` 436), the packet and VSync events that
place them in time, and a cheap kernel set (DPCs, interrupts, context switches, the dispatcher's ready events)
that says whether a decision ran inside a DPC or on a thread somebody woke.

The mode is deliberately narrow:

- it needs the CPU window, so `etw-start.ps1` refuses it with `-FpsSeconds` and asks for `-WorldSeconds N`;
- it drops CPU sampling and every disk group (`/Profile` alone cost 1.6 ms a frame on the lab, which moves the
  rate by about 10 %), adds `/StackCompression`, and asks for stacks on three event ids only. The id filter is
  **additive**, not restrictive: PerfView and TraceEvent both document `@EventIDsToEnable` as collected "in
  addition to any events specified by the Keywords", so the keyword mask still sets the base volume (about
  19 MB/s on RotTR D3D12) and what the mode narrows is the stack walking. The volume of the stacked stream is
  therefore not predicted here; the smoke run below measures it;
- both id lists are **space** separated and the whole `/Providers` value is one quoted argument, because
  `/Providers` is itself a comma-separated list of provider specifications. A comma inside an id list splits the
  spec into providers named `238`, `436` and so on, after which PerfView enables one event id and no stack filter
  at all, with nothing at run time to say so. `dryrun-args.ps1` round-trips the spec through `Start-Process` and
  Windows argv splitting, and `host-checks.ps1` refuses a comma in either list;
- it changes nothing about the `logman` `-gpu` session. That session stays the unfiltered packet-level source
  the gap table is built from, so a scheduler-stack window is still comparable with every earlier window;
- the reserve goes to 35 s, because a stacked session has more buffers to flush, and `-ReserveSeconds` reaches
  the task exactly once.

The control it owes: stalls per second, median gap and the VSync-ended share of the stacked window must match a
plain `-FpsSeconds` window of the same session within 20 %. If they do not, the instrument changed the thing it
was measuring and the mechanism it names is not evidence.

Before the first game window, run `etw-capture.ps1 -Smoke -SchedulerStacks` once with no game and read three
numbers out of `etw-notes.txt`: the collector's exit code (a refused provider spelling shows up there and
nowhere else), the stacked ETL's size in bytes over the window's seconds, and - from the file itself - that it
holds events of id 436 **with** a stack. Without that last count the mode can look healthy and still have
walked no stack at all, which is the one thing it exists for. Also confirm the keyword values of this Windows
build with `logman query providers Microsoft-Windows-DxgKrnl`: the mask 0x88008001 is
Base | GPUScheduler | Present | Deprecated as that command prints them.

## What it needs

- A game trial that has started, with a `start.json` and a game stage on the lab. `etw-start.ps1` refuses a
  stage that is not a game trial, a trial that has not started, a trial with less than 60 s left, and a trial
  directory that already holds a capture.
- The staged, hash-checked PerfView on the lab, at `C:\BC250\tools\perfview\PerfView.exe`. The start script
  refuses any other copy.
- The lab configuration outside this repository (`tools/win/README.md`). No address and no key is here.
- Space for the captures. A 30 s GPU session is hundreds of megabytes, and a CPU profile is more. The data
  stays on the lab and in the workspace scratch area, never in this repository.

## Hazards

- **The capture is an instrument inside a live trial, and it can cost the trial.** A syntax error, an unknown
  switch or a missing deadline ends as a failed task with nothing to read. Run `host-checks.ps1` before a
  trial, after every edit.
- **The start script passes switches that the staged copy must know.** The lab copy under `C:\BC250\tmp` is a
  push of this directory, not a second source. A staged copy without `-Process` or `-PresentMode` makes the
  start script refuse rather than start a capture that ignores them.
- **A capture with the CPU profile on changes the frame rate it measures.** Use `-GpuOnly` for every rate
  number, and say in the record which mode produced a figure.
- **Task exit is not a closure witness.** Always run `etw-closure.ps1`, and read its exit code. An ETW session
  left running on the lab is the one failure of this tool that outlives the trial.
- The `-Process` names come from the game profile of the trial stage. Witcher 3 is the default, so its task
  line looks the same as before the profiles existed.
- **`-SchedulerStacks` is a mechanism instrument, not a rate instrument.** Its window carries context
  switches and stack walks, so no frame rate from it is a rate number. It owes the 20 % control against a
  plain window of the same session before anything it shows is read as evidence.
- **That control ran, and it failed (K189).** In session 428 the stacked window moved the very thing it
  measures: the share of long node-0 stalls that end at a VSync fell 47.7 %, the recorded end of a stall came
  about 180 us late, and the slice frame rate was 69.35/s against 74.23/s in the plain window of the same
  session. So read this mode for which call site lifted the gate, never for how often or how long. A number
  that needs a rate or a share comes from a plain window.
