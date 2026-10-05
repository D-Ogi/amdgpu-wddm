# gpu-timeline - what the GPU did, at 5 kHz

A frame rate says that the GPU is slow. It does not say why. This tool samples the GRBM block-activity and
command-processor status registers through `bc250rd` at up to 5 kHz and splits the time of a run into idle,
pipeline work and command processor alone, and it names what the command processor waits on. It is the only
instrument in the workspace that sees a command-processor wait while the GPU is busy, which a frame counter
and an ETW trace both miss.

Reads only. The sampler sends `bc250rd`'s ATTACH and READ requests, never its SMU or SMN requests, and no
register in its sets changes state when it is read.

## Where the register offsets come from

`gtl_regs.h` is generated, never typed. `gen_regs.py` asks `tools/regcalc` for every BAR5 offset, as
`docs/02-register-addressing.md` requires, and then refuses the set unless each offset is also in
`tools/win/bc250rd/driver/allowlist.h`, because the driver refuses a whole batch when one offset is not in
its allow-list. It also refuses a register whose read changes state (facts M25: `*_HEADER_DUMP`,
`*INVALIDATE_ENG*_SEM`, `GRBM_GFX_CNTL`). `test_analyze.py` checks every offset of both sets against
`regcalc` again, so a hand-edited header fails the gate.

`GRBM_STATUS` is read first and again last in one sample. The analyzer counts a sample whose class differs
between the two reads as a "bracket change", which is the error bar of the point-sample assumption.

## What is here

| File | What it does |
|---|---|
| `gpu-timeline.c` | The sampler: one batch of register reads per period at the asked rate, into a GTL1 file, with a probe, a bounded run and a `selftest` that opens no device |
| `gtl_regs.h` | The generated register sets: 17 registers in the full set, 9 in the lite set |
| `gen_regs.py` | Writes `gtl_regs.h` from `tools/regcalc`, against `bc250rd`'s allow-list and the M25 deny list |
| `build.ps1` | Builds the sampler, writes the provenance of the exact artifact, and runs every gate below |
| `analyze.py` | The split per second and per frame: idle, pipeline (fed or draining), command processor alone with its reason (sync, drain, poll, write, fetch, parse, other), and the top raw bit combinations of each class |
| `test_analyze.py` | The analyzer's unit tests: the C selftest file with exact shares, a random timeline with known truth sampled as the lab samples it, the lite set, failed reads and the file format |
| `gtl-host.py` | The development-PC side: push, run, stop and pull, through `tools/win/target.py` |
| `lab/gtl-run.ps1` | The lab side: the preflight, the probe, one bounded run, and a receipt with the hashes |
| `lab/test-invoke-gtl.ps1` | The host check of the lab wrapper's process handling under Windows PowerShell 5.1, the lab's shell |

## Build

```powershell
pwsh tools\win\gpu-timeline\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Out <BC250_ROOT>\scratch\build\gpu-timeline
```

The compiler comes from the installed Visual Studio, the headers and import libraries from the SDK NuGet
packages. The artifacts stay outside this repository. `/W4 /WX` and `/Brepro`: the same sources and toolchain
give the same hash, and `build-info.txt` records the hashes of what went in, of the exe, and the repository
revision. The build stops at the first failure of its gates: `gen_regs.py`, the compile, the exe's
`selftest`, the analyzer's tests, and the lab wrapper's check.

`BC250_GTL_BUILD` names the build directory for the tests and for the lab wrapper's check, and `build.ps1`
sets it to its own `-Out`. Without the exe the analyzer's selftest case skips itself, so the `gpu-timeline`
check of `tools/quality/quick.ps1` passes on a PC that has not built the tool.

## Run

```
python tools/win/gpu-timeline/gtl-host.py push                     the exe and the lab script
python tools/win/gpu-timeline/gtl-host.py run 310-low --seconds 60 one bounded run (--detach: no held session)
python tools/win/gpu-timeline/gtl-host.py stop 310-low             end it early; it still writes its file
python tools/win/gpu-timeline/gtl-host.py pull 310-low             the receipt, the texts and run.gtl
python tools/win/gpu-timeline/analyze.py <runs>/310-low/run.gtl --fps 59.9 --json result.json
python -m unittest discover -s tools/win/gpu-timeline -p "test_*.py"
```

`--fps` turns the shares into milliseconds per frame. `--frames` takes the frame boundaries on the QPC clock
and gives a row per frame, which is how a hitch is found in one frame instead of one second.

## Hazards

- **`bc250rd` must run on the lab**, and the sampler needs its driver, not only its CLI. The preflight records
  the driver's path and hash, and a probe without the driver ends with exit code 1.
- **One run is at most 60 s** and the host refuses more. A run holds an ssh session for its length unless
  `--detach` makes it a one-shot SYSTEM task.
- **The exe is pinned.** `gtl-host.py` passes the local hash as `-ExpectExeSha256`, so the lab refuses a stale
  copy. The output directory must be new: a receipt never overwrites an earlier run.
- **The sample is a point sample.** A missed period that correlates with the state biases the shares, so the
  analyzer weights the samples by the gap, and a test holds that correction.
- **The bit names are AMD's.** What a bit means under load is not documented beyond its name, so the raw bit
  combinations are printed with every class and an interpretation can change without a new run.
- The runs and the exe stay in the workspace scratch directory. One 60 s run at 1009 Hz with the full set is
  about 4.7 MB, and the workspace drive filled up once with analysis output.

## The scratch copy

The operators still call the copy in the workspace scratch directory, together with its captures. This copy
is the source of record.
