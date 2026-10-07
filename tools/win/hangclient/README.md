# hangclient - the deliberate GPU hang of M15.12

`bc250hang.exe` is the client the M15.12 hang-recovery trial needs: a native D3D12 program that makes the GPU
stop, on purpose, in the one way stage-1 soft recovery is built for. It is ours, not a game: a trial that proves
recovery has to be able to repeat the hang, and nothing else here can.

It takes the **system** D3D12 runtime (`System32\d3d12.dll`) and the BC-250 adapter (PCI `1002:13FE`), as
`tools/win/d3d12queue` does. No window, no resident state, no application-local runtime. The compute shader is
DXBC `cs_5_0`, compiled at run time by `d3dcompiler_47.dll`.

```
bc250hang (--lab|--warp) (--short|--long) [--millis N] [--deadline S]
```

| Mode | What it does | What proves it worked |
|---|---|---|
| `--lab --short` | One dispatch calibrated to about 300 ms, under the kernel driver's 500 ms submit watchdog (`BC250_SUBMIT_POLL_US`). The **positive control**: the GPU really ran our work and the device is still alive | `RESULT=SHORT-COMPLETED-DEVICE-ALIVE`, exit 0 |
| `--lab --long` | **One** unbounded dispatch, one device, one `ExecuteCommandLists`, no calibration and no retry. The waves never retire, the end-of-pipe never fires, so dxgkrnl runs its per-engine TDR about `TdrDelay` seconds later | `RESULT=HANG-DETECTED-DEVICE-REMOVED` with `reason=0x887a0006` (`DXGI_ERROR_DEVICE_HUNG`) or `0x887a0005` |
| `--warp ...` | Build and smoke mode on any machine, the development PC included: it forces a tiny completing dispatch, so it can never peg a CPU-only device. `--warp --long` exercises the long path's polling, grace and exit and must end `RESULT=FENCE-COMPLETED-NO-REMOVAL` | `RESULT=WARP-SMOKE-OK` for `--short` |

`--millis N` moves the `--short` target (at most 450 ms, so the control can never trip the watchdog itself).
`--deadline S` bounds the `--long` wait (12 to 120 s, default 25), so the client cannot outlive a lab trial.

Build (the repository's own recipe, output in the workspace scratch tree, never on C:):

```
pwsh -NoProfile -File tools\win\hangclient\build.ps1 -Kits P:\bc-250\toolchain\nuget `
     -Out P:\bc-250\scratch\hang-recovery\build-b22\hangclient
```

The build keeps the previous binary under `retained\<sha256>.exe`, runs `--help` and refuses an invalid command
line, and prints the size and SHA-256 of what it made.

## Why a spin loop and not an unmapped read

The hang class of the real bugcheck reports (trials 147, 151, 208, 245, 251) is a no-retry GFXHUB fault whose
waves stop on `MEM_VIOL`. A read of never-mapped memory cannot be formed through the validated system D3D12
runtime without a descriptor the runtime rejects, so this client does not try. The spin loop reproduces the
class the stage-1 kill is for: live waves, which `SQ_CMD` `CMD=KILL` stops, after which the end-of-pipe
fires and the fence retires. Whether the same kill drains `MEM_VIOL` waves that have already stopped on gfx1013 is
an open question, and `docs/linux-session-wishlist.md` row L43 is how to settle it. A pass here is therefore not a claim
about the reports' class - that limit is written down in `docs/design/hang-recovery.md`.

Nie wszystko złoto, co się świeci - not all that glitters is gold: a recovered device is not a recovered
hang class.
