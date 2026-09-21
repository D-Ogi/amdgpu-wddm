# driver/kmd: bc250kmd, the WDDM miniport

State: **milestones M3 and M4 reached on unit A** (experiment E06, facts M28 and M29): the driver is installed on the
lab machine and runs its display. The acceptance list below is met; escape reaches the driver. M5's first part
(0.5.2): the PSP takes the GPU firmware from this driver (experiment E10, facts M34 and M35).

What it is (ADR 0006): a display-only miniport for `PCI\VEN_1002&DEV_13FE` that takes over the firmware's
framebuffer through post-display ownership, offers exactly the mode the firmware left, and presents by CPU
copy. The display path performs **no MMIO** and enables no interrupt; the bring-up code next to it (ADR 0007)
touches hardware only behind registry gates that default to 0 and that every install closes. Written from
the documented DDI; no code from Microsoft's MS-PL sample.

| File | Contents |
|---|---|
| `entry.c` | `DriverEntry`, the display-only DDI table |
| `pnp.c` | add/start/stop/remove, the single child (always-connected video output, no EDID), power |
| `display.c` | VidPN (one source, one target, one mode, identity only), `PresentDisplayOnly`, bugcheck display, the escape query |
| `guard.c` | boot-loop guard, stage breadcrumbs in the registry, log |
| `mmio.c`, `gen_regs.py`, `regs.generated.h` | BAR5 behind `EnableMmio` / `EnableMmioWrite`; every access checked against tables generated through regcalc (E07) |
| `sequence.c` | the kernel backend of `driver/shim`, shared by every bring-up sequence: registers only through the sequence's own generated table, first refused access stops all further writes, a plan executes no write and records what would be written |
| `gart.c` | M4: the GART command (plan, enable, restore) around AMD's imported hub code, behind `EnableGart`; registers through a table generated from amdgpu's own trace of the step (E09) |
| `psp.c` | M5, first part: the PSP command (plan, load, unload) around AMD's imported `psp_v11_0_8.c` and `driver/shim/bc250_psp.c`, behind `EnablePsp`: PSP ring, trusted memory region and the ten firmware images, staged in the top of VRAM from files in `C:\BC250\firmware` (E10) |
| `vram.c` | the VRAM carve-out by system physical address and through BAR0, behind `EnableVram` / `EnableVramWrite`; one page per access, writes only in a test page (E08) |
| `bc250kmd_escape.h` | private escape data shared with lab tools |
| `bc250kmd.inf`, `build.ps1` | package and build (direct `cl`/`link`, `/W4 /WX`, test-signed) |

## Built for a lab without a kernel debugger

- **Start budget.** `Services\bc250kmd\Parameters\UnconfirmedStarts` is incremented at every
  `DxgkDdiStartDevice`. At 2 the driver refuses to start and Windows falls back to the Basic Display driver.
  User mode confirms a good start by writing 0: `bc250mon`'s `KmdProvider` does it by itself once the desktop
  has been up for 60 s and `LastStage` has reached 61, and by hand it is `bc250kmd_cli confirm`
  (`tools/win/bc250kmd_cli`) or `mon.py action kmd.confirm`. Installing the package resets it.
- **Breadcrumbs.** `LastStage` (a `BC250_STAGE` number) and `StageHistory` in the same key are written and
  flushed at every step of start-up and at the first commit and first present. After a hang and a power
  cycle they say how far the driver got. `bc250mon`'s bc250kmd panel and `bc250kmd_cli stages` read them and
  name them; both keep a copy of the `BC250_STAGE` table that a test checks against this driver's header.
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
