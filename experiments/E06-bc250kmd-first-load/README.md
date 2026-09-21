# E06: first load of our own miniport (bc250kmd, milestone M3)

State: **run 001 done, H1-H4 hold, H5 answered: yes** (2026-09-21). Evidence: `evidence/windows/2026-09-21-E06-run-001/`.

## Why

E05 showed with Microsoft's sample that a display-only driver can own `1002:13FE` and live on the firmware's
framebuffer (facts M26). `driver/kmd` is our own miniport of the same shape, written from the documented DDI,
plus the lab scaffolding (start budget, stage breadcrumbs, escape query). It has never been loaded. This
experiment is its first contact with hardware, and the acceptance test of milestone M3
(`driver/kmd/README.md`).

The driver performs no MMIO, so the hardware cannot be harmed by it. What can go wrong is software: a
bugcheck, a black screen, a failed start.

## Hypotheses

- H1. The package installs on `PCI\VEN_1002&DEV_13FE` without a reboot, the device starts without a problem
  code and the desktop stays visible at the firmware's mode.
- H2. `LastStage` reaches 61 (first present done) and `StageHistory` shows the stages in order
  10, 20, 30..39, 50, 60, 61.
- H3. A GC sweep with `bc250rd` (corrected allow-list, facts M25) before and after shows no change outside
  the noise set of two unchanged sweeps.
- H4. The driver survives disable/enable and a restart; after the restart `bc250mon` confirms the start and
  `UnconfirmedStarts` returns to 0. Uninstall brings Basic Display back.
- H5 (measured either way, no expectation): `D3DKMTEscape` with our `BC250_ESCAPE_GET_INFO` reaches
  `DxgkDdiEscape` of a display-only driver. Baseline without an escape of ours: `STATUS_INVALID_PARAMETER`
  from Basic Display, `STATUS_NOT_SUPPORTED` from the KMDOD sample
  (`evidence/windows/2026-09-21-escape-probe/`).

What would refute: a problem code or a fallback to Basic Display (H1), a `LastStage` below 61 with a visible
desktop or stages out of order (H2), any register outside the noise set (H3), `UnconfirmedStarts` stuck at 1
or a refusal at the second start (H4).

## Recovery

As in E05 (all three paths; the safe-mode entry over the wired NIC is proven). In addition the driver's own
start budget: after two unconfirmed starts it refuses and Windows falls back to Basic Display.

## Procedure

The E05 target script drives this run as well, pointed at our package:

```
e05_target.ps1 -Package C:\BC250\e06 -InfName bc250kmd.inf -Phase state|sweep|install|cycle|rollback
```

1. Code review of `driver/kmd` by a second reader before the load; findings fixed and rebuilt.
2. Announce on the overlay. State, two GC sweeps (noise floor).
3. Install. State, `bc250kmd_cli stages`, `bc250kmd_cli info` (H5), screenshot, GC sweep.
4. Disable/enable. Stages again (a second start within one boot: the budget counts it).
5. Restart. State, stages, `UnconfirmedStarts` after the monitor's confirmation.
6. Rollback unless everything holds; if everything holds the driver stays installed for M4 work and the
   rollback is proven once and the package installed again.

## Result (run 001)

Before the load a second reader went through `driver/kmd` and found five things; four were fixed first
(no uncached fallback when the write-combined mapping of the framebuffer is refused; `CommitVidPn` returned
the success-severity `STATUS_GRAPHICS_MODE_NOT_PINNED` as if it were an error; presents were not tied to a
validated commit, so a source surface of another size could have been read out of bounds; one field of the
recommended monitor mode differed from the proven value). The fifth, that only user mode resets the start
budget, is the design (ADR 0006 point 3) and stays; see the last row.

No kernel debugger was attached (the debugger server had hung the development PC earlier that day, journal).

| | Outcome |
|---|---|
| H1 install, no problem code, mode kept | **holds**: `pnputil /add-driver /install`, no reboot, `CM_PROB_NONE`, 1920x1200; the owner confirms the picture fills the panel |
| H2 stages | **holds**: `LastStage` 61, history `10 20 30 31 32 33 34 39 50 60 61` after every start |
| H3 no register change | **holds**: noise floor 9 registers (it was 36 before the side-effect registers left the allow-list), 0 outside it after the install |
| H4 disable/enable, restart, budget, rollback | **holds**: both come back at stage 61; the restart ran at the edge of the budget (1 -> 2) and `bc250mon` confirmed it to 0 about a minute after the desktop; rollback returned Basic Display; the package was installed again and stays |
| H5 escape | **yes**: `D3DKMTEscape` reaches `DxgkDdiEscape` of a display-only driver, `STATUS_SUCCESS`, data correct (facts M29). Escape is the control channel for M4 |

Found on the way:

- `bc250mon` confirms a start once per boot. A second start within the same boot (disable/enable, re-install)
  stays unconfirmed until the next boot, so two device cycles in one boot would run into the guard. To change:
  confirm again whenever stage 61 has been stable for 60 s.
- The monitor process keeps the DPI it started with. After the driver switch Windows went from 200 % back to
  100 % and captures came out as a quarter of a 3840x2400 canvas until the monitor was restarted.
- `GRBM_READ_ERROR` still latches during a sweep (now with the address of `0x8090`, the register after the
  excluded `GRBM_GFX_CNTL`), and it read 0 again at the start of the next sweep although nothing of ours
  clears it. Which reads fault, and what clears the latch, is open.
