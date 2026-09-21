# E06: first load of our own miniport (bc250kmd, milestone M3)

State: **prepared, not run.**

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

## Result

Not run yet.
