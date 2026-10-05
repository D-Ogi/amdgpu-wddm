# capture-share - capture and shared-surface witness (M15.13)

`capshare.exe` tests desktop capture, window capture and cross-process shared resources on one adapter. Each run
tests one cell and prints one verdict line. Criterion M15.13 in `docs/m15-reconciliation.md` needs these results
while the desktop runs on the GPU. Criterion M15.11 (hardware video encode) needs the same capture input path.

The client has two parts in each cell. The parent process is side A. It creates the shared object, or it is the
capture consumer. The peer process is side B. It opens the shared object, or it is the window producer. Both
processes write into the same text log. `capshare-peer.exe` is a byte copy of `capshare.exe` with a second image
name. The application router sends each image name to its own D3D11 user-mode driver, so the two processes of one
cell can run on different drivers.

## What a run reports

Each run writes the text log that `--out` names and the JSON file that `--json` names. The last line of the log is
the verdict:

```
VERDICT cell=s12to11 result=pass side=- stage=- call=- hr=- at=- got=- want=- diff=0/0 max_delta=0
        content=- gate=held route=A:...,B:... fl=A:12_2,B:12_1 checks=B:P0=pass,B:A=pass,A:B=pass
        elapsed_ms=1031 note=-
```

| Field | Meaning |
|---|---|
| `result` | `pass`, `mismatch` (the image or the gate was wrong), `fail` (a call failed) or `timeout` |
| `side` | which process decided the result |
| `stage` | the step that was active |
| `call` | the call that failed, with `hr` |
| `at`, `got`, `want`, `diff`, `max_delta`, `content` | the first wrong pixel and what the image held instead |
| `gate` | `held` when no side got ahead of its synchronisation, or `violated:<step>` |
| `route`, `fl` | the loaded user-mode drivers and the feature level of each side |
| `checks` | every oracle result of the run |

Exit codes are 0 for pass, 1 for mismatch, 2 for fail, 3 for timeout and 4 for a bad argument. A watchdog ends
every run inside `--bound` seconds and reports the stage that hung. `--bound` accepts 1 to 170 seconds.

## The cells

`capshare.exe --help` lists them. The pass condition of each cell is in the last column.

| Cell | Processes | What the cell checks |
|---|---|---|
| `km11` | D3D11 to D3D11 | A shared keyed-mutex texture, opened with `OpenSharedResource1`. The keys 0 to 4 pass the texture between the two processes. `--handle kmt` uses the legacy global handle instead of an NT handle. Pass: the opener reads the poison image, then image A, and the creator reads image B. |
| `km12to11` | D3D12 to D3D11 | The same handshake on a texture from `ID3D12CompatibilityDevice::CreateSharedResource`. The D3D12 side reaches the keyed mutex through D3D11On12. |
| `km11to12` | D3D11 to D3D12 | The same handshake in the other direction, with `ID3D12Device::OpenSharedHandle`. |
| `s11to11` | D3D11 to D3D11 | A shared texture without a keyed mutex. Each side waits for its own GPU work on the CPU. Pass: both sides read the image the other side wrote. This cell checks the open and the memory alias alone. |
| `s12to11` | D3D12 to D3D11 | `ID3D12Device::CreateSharedHandle` to `ID3D11Device1::OpenSharedResource1`. |
| `s11to12` | D3D11 to D3D12 | `IDXGIResource1::CreateSharedHandle` to `ID3D12Device::OpenSharedHandle`. |
| `s12to12` | D3D12 to D3D12 | A shared texture between two processes of the same API. `--simultaneous` adds `ALLOW_SIMULTANEOUS_ACCESS`. |
| the four `s` cells with `--sync fence` | as above | The same texture, but the GPU order comes from two shared fences instead of CPU waits. Pass needs the images and both gates. |
| `f11to11`, `f12to11`, `f11to12`, `f12to12` | each API pair | Two shared fences and no shared texture. Each side signals one fence and waits on the other fence on the GPU. Pass: both fences reach 2, and each side gets past its wait only after the other side signals. |
| `w11` | one process | Control without sharing. A local D3D11 fence signal and a wait on one context. |
| `w12` | one process | Control without sharing. Three local D3D12 queue waits. The first value is already reached. The CPU signals the second value later. A second queue signals the third value. |
| `ipc` | two processes | The harness alone. It creates the peer process, the pipe and a duplicated event handle. It creates no device. |
| `capdry` | one process | The capture oracle alone, with no capture. It builds a synthetic desktop texture and copies the window image into it. The region compare must find the image, report a 3,2 displacement as `shift=-3,-2` and name a stale image. Both 8-bit channel orders. |
| `dda` | two processes | Desktop Duplication. The producer shows image A in a borderless window. The D3D11 consumer acquires frames and compares the window rectangle. Then the producer switches to image B and the consumer must see B. A window appears, so this cell needs `--interactive-ok`. |
| `wgc` | two processes | The same oracle through Windows.Graphics.Capture, with `GraphicsCaptureItem` from the window handle and a free-threaded frame pool. This cell also needs `--interactive-ok`. |

### How a cell can tell a real wait from luck

The creator fills the shared texture with a poison image first. The opener must read that poison image, which
proves that the open aliases the creator's memory. Every pattern write runs behind a delay workload of large
texture copies, so a reader that does not wait for the writer sees poison or the image from the step before.
Each cell also holds a gate: the waiting side reports its fence value or its mutex key after `--gate-ms`
milliseconds, and a side that got ahead of its synchronisation makes the run a mismatch with `gate=violated`.

`--inject skip-wait` is the negative control. The opener then leaves out its first GPU wait. The run must end in
`mismatch` with `gate=violated`. A run of the `f` cells or of the `s` cells with `--sync fence` that passes with
this switch proves that the gate measures nothing.

## Build

```powershell
pwsh tools\win\capture-share\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget
```

The build takes headers and import libraries from the SDK NuGet packages under `-Kits`, with the C++/WinRT
projection that the `wgc` cell needs. The compiler comes from the installed Visual Studio. `/W4 /WX`, x64, `/MT`.
`-Out` defaults to `<workspace>\scratch\build\capture-share`. Temporary files of the compiler stay under
`<workspace>\scratch\tmp`. The build copies `capshare.exe` to `capshare-peer.exe` and writes `SHA256SUMS.txt`.

## Offline validation on a development PC

```powershell
pwsh -NoProfile -File tools\win\capture-share\host-validate.ps1
```

The script runs the 32 headless checks and compares each verdict with the expected one. It runs the self-test, the
harness cell, both controls, the oracle check, every keyed-mutex, shared-texture and fence cell, four negative
controls with `--inject skip-wait`, and the interlock check that `dda` and `wgc` refuse to start without
`--interactive-ok`. It opens no window. It writes `<Out>\<stamp>\summary.txt` with one line for each run.

A development PC with a working graphics driver ran all 32 checks as expected on 2026-10-01. That result proves
the client and the oracles. It says nothing about unit A.

## Run it on the lab

Every command goes through `tools\win\target.py`, which picks the address and the SSH options. Never write a lab
address into a command.

```
python tools\win\target.py push <workspace>\scratch\build\capture-share\capshare.exe --to C:\BC250\tmp
python tools\win\target.py push <workspace>\scratch\build\capture-share\capshare-peer.exe --to C:\BC250\tmp
python tools\win\target.py run "C:\BC250\tmp\capshare.exe --cell s12to11 --bound 60 --out C:\BC250\tmp\s12to11.txt --json C:\BC250\tmp\s12to11.json"
python tools\win\target.py pull C:\BC250\tmp\s12to11.txt <evidence directory>\s12to11.txt
```

Rules for a lab run:

- An SSH session runs in session 0, which has no desktop. The cells `km*`, `s*`, `f*`, `w11`, `w12`, `ipc` and
  `capdry` need no desktop, so `target.py run` is enough for them.
- The cells `dda` and `wgc` show a window and read the screen. They must run in the logged-on session. Start them
  as a scheduled task with an interactive principal, as `tools\win\d3d12queue\scanout-trial.ps1` does for its own
  client. Keep the task name clear of the deploy kits' competing-task pattern, and remove the task after the run.
- Each lab trial stays inside three minutes. Give `--bound` a value that ends the run first.
- `--peer-exe capshare-peer.exe` sends the peer process to the second image name. Put only one of the two names on
  the application router's allowlist to give the two processes different D3D11 drivers.
- `--stderr <file>` relaunches the client with that file as its standard output and error from the start. The
  diagnostics of every user-mode driver in both processes land there. Collect that file together with the log and
  the JSON after a failure.
- `--dbwin dwm.exe` also records the debug lines of the compositor during the run.
- A failure report needs four files: the `--out` log, the `--json` verdict, the `--stderr` file and the kernel
  driver log around the run.

Expect a cross-driver open to be the first wall. Desktop Duplication hands the consumer a surface that the
compositor's user-mode driver owns. The verdict line names the failing call and the removed reason of the device,
so the result says where the stop is.

## Status

The lab run under GPU desktop composition is not done. Row M15.13 of `docs/m15-reconciliation.md` stays "not
measured on the GPU route" until a lab run exists. The run and that row are later steps.
