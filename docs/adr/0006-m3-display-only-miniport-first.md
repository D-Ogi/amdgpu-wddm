# ADR 0006: M3 is a display-only miniport of our own, built for a lab without a kernel debugger

Date: 2026-09-21. Status: **accepted** the same day. The condition was that experiment E05 shows that a
display-only driver can own `1002:13FE` on unit A and keep the firmware's display; run 001 showed it (facts M26).

## Context

- Milestone M3 asks for a WDDM miniport that binds to the GPU and keeps the screen alive. Nothing in M3
  needs an engine: the firmware has already trained the DisplayPort link and set a mode, and amdgpu's own
  init shows that display bring-up is two thirds of all register traffic (facts M24). We want none of that yet.
- WDDM offers exactly this shape: a kernel-mode display-only driver (`DxgkInitializeDisplayOnlyDriver`),
  which takes over the firmware framebuffer through `DxgkCbAcquirePostDisplayOwnership` and presents by
  copying into it. Microsoft's KMDOD sample does it; E05 runs that sample, unmodified, as an instrument.
- The sample is MS-PL. Our repository is PolyForm Noncommercial (ADR 0004). We do not mix them: the sample
  stays outside the repo, and our miniport is written from the documented DDI, with the sample's observed
  behaviour on this board as the acceptance test.
- The lab has no kernel debugger (the target is reachable over a USB Wi-Fi dongle only, KDNET needs the
  wired NIC), a bugcheck stays on screen until the owner power-cycles the machine, and the M.2 link does not
  always come back after a restart (facts M21). Every hang costs the owner a trip. The driver has to be
  built for that.

## Decision

1. **M3 is a display-only miniport written by us in C**, in `driver/kmd/`, built with the same direct
   `cl` / `link` flow as `tools/win/bc250rd` (no Visual Studio project system), test-signed with the lab
   certificate. One source mode, the one the firmware left; present is a CPU copy into the framebuffer
   reported by post-display ownership. DDIs we do not implement return an honest failure code.
2. **M3 performs no MMIO.** It maps nothing but the framebuffer. `tools/win/bc250rd` remains the instrument
   for register reads and the SMU mailbox while M3 is in use. Hardware access enters the miniport in M4,
   one path at a time, each behind a registry gate that defaults to off (driver/README rule).
3. **Boot-loop guard.** `DxgkDdiStartDevice` counts start attempts in the driver's registry key before it
   does anything else; user mode (`bc250mon`, once the desktop is up) clears the counter. At two uncleared
   starts the driver refuses to start and Windows falls back to the Basic Display driver. A broken build
   can therefore cost one bad boot, not a dead machine.
4. **Breadcrumbs instead of a debugger.** The driver writes a "last stage reached" value to its registry key
   at each step of start-up and each first use of a DDI (cheap, synchronous enough, survives a power cycle),
   and logs through a TraceLogging provider captured by an autologger session to a file. `bc250mon` gets a
   provider that shows both on the overlay.
5. **One install path, one rollback path, both scripts**: `pnputil /add-driver ... /install` and
   `pnputil /delete-driver ... /uninstall /force`, wrapped so that each install first records the state
   (device, driver store entry, register sweep) and announces itself on the overlay. The recovery boot
   entry (safe mode with networking, `sshd` allowed) is proven before the first install and stays.
6. **From display-only to full WDDM is a rewrite of the entry point, not of the driver.** Display-only and
   full miniports share PnP start/stop, interrupt, child and VidPN code. The pieces are kept in separate
   files from the start (`pnp`, `display`, later `memory`, `engine`), so that M7 swaps
   `DxgkInitializeDisplayOnlyDriver` for `DxgkInitialize` and adds VidMm/VidSch DDIs without touching
   the display part.

## Open questions (to settle with measurements, not here)

- How experiments in M4-M6 (GART, interrupt ring, first SDMA command) are driven from user mode while the
  miniport owns the device. `KMDDOD_INITIALIZATION_DATA` has a `DxgkDdiEscape` slot (`dispmprt.h`, WDK
  10.0.26100); whether `D3DKMTEscape` from an elevated tool actually reaches a display-only driver is to be
  measured with the M3 driver itself (a version query, nothing more). If it does, escape is the control
  channel. If not: a registry-driven self-test at start, or a second, software-only control driver like
  `bc250rd` that shares no state. Decide at the start of M4.
- Whether firmware (PSP-loaded microcode) survives a warm restart into Windows (wishlist L2). It decides
  whether M5 needs a PSP path first.
- Whether the owner can connect the wired NIC, which would give us KDNET and make point 4 a convenience
  instead of the only source of truth.

## Update, 2026-09-21 (after acceptance; the text above is left as decided)

- The owner connected the wired NIC. KDNET works over the on-board RTL8168 and the recovery boot entry has
  network and SSH over that path (facts M27). The context line "the lab has no kernel debugger" is no longer
  true. Points 3 and 4 stay: the guard protects against boot loops with or without a debugger, and the
  breadcrumbs are what the owner sees on the overlay and what survives when no debugger is attached.
- Baseline for the escape question, measured before our driver exists: `D3DKMTEscape` with a driver-private
  request returns `STATUS_INVALID_PARAMETER` from Basic Display and `STATUS_NOT_SUPPORTED` from the KMDOD
  sample on the same device (`evidence/windows/2026-09-21-escape-probe/`). Neither implements an escape of
  ours, so this does not answer the question; the M3 driver does.

## Consequences

- The first driver of ours that binds to the GPU is small enough to review line by line, and it cannot hurt
  the hardware: it writes to no register.
- We accept that M3 looks like "just another KMDOD". Its value is the scaffolding around it: guard,
  breadcrumbs, install/rollback, overlay integration. Those carry every later milestone.
- Desktop composition stays on WARP until a user-mode driver exists (ADR 0005). That is expected and is not
  a regression against the Basic Display driver the machine runs today.
