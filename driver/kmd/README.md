# driver/kmd: bc250kmd, the WDDM miniport

State: **milestone M3 skeleton, builds, never loaded.** It is not installed on any machine until experiment
E05 (Microsoft's known-good display-only sample on the same device) has run and ADR 0006 is accepted.

What it is (ADR 0006): a display-only miniport for `PCI\VEN_1002&DEV_13FE` that takes over the firmware's
framebuffer through post-display ownership, offers exactly the mode the firmware left, and presents by CPU
copy. It performs **no MMIO** and enables no interrupt. Written from the documented DDI; no code from
Microsoft's MS-PL sample.

| File | Contents |
|---|---|
| `entry.c` | `DriverEntry`, the display-only DDI table |
| `pnp.c` | add/start/stop/remove, the single child (always-connected video output, no EDID), power |
| `display.c` | VidPN (one source, one target, one mode, identity only), `PresentDisplayOnly`, bugcheck display, the escape query |
| `guard.c` | boot-loop guard, stage breadcrumbs in the registry, log |
| `bc250kmd_escape.h` | private escape data shared with lab tools |
| `bc250kmd.inf`, `build.ps1` | package and build (direct `cl`/`link`, `/W4 /WX`, test-signed) |

## Built for a lab without a kernel debugger

- **Start budget.** `Services\bc250kmd\Parameters\UnconfirmedStarts` is incremented at every
  `DxgkDdiStartDevice`. At 2 the driver refuses to start and Windows falls back to the Basic Display driver.
  User mode confirms a good start by writing 0 (to become a `bc250mon` provider; by hand:
  `reg add HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters /v UnconfirmedStarts /t REG_DWORD /d 0 /f`).
  Installing the package resets it.
- **Breadcrumbs.** `LastStage` (a `BC250_STAGE` number) and `StageHistory` in the same key are written and
  flushed at every step of start-up and at the first commit and first present. After a hang and a power
  cycle they say how far the driver got.
- The service is `ErrorControl = 0`: a failed start never stops the boot.

## Build

```powershell
pwsh driver\kmd\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd
```

## What M3 has to show on hardware (acceptance, mirrors E05)

1. Installs on the device, starts without a problem code, desktop stays visible at the firmware's mode.
2. `LastStage` reaches 61 (first present done).
3. A BAR5 sweep with `bc250rd` before and after shows no change caused by the driver.
4. Survives disable/enable and a reboot; uninstall brings Basic Display back on the same framebuffer.
5. Measured, either way: does `D3DKMTEscape` reach a display-only driver (decides the control channel for M4).
