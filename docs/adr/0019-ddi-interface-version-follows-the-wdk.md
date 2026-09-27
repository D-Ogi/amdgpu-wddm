# ADR 0019: the kernel driver compiles and declares the newest WDDM DDI version the WDK offers

Date: 2026-09-27. Status: accepted direction (owner decision of 2026-09-27: "it should definitely be a higher
version, we want bleeding edge"). Supersedes ADR 0008 point 2. The move itself is engineering work with its own
experiments; nothing here is measured unless it cites a `facts.md` row.

## Context

ADR 0008 point 2 (2026-09-21) fixed the binary at `DXGKDDI_INTERFACE_VERSION_WDDM2_0` (0x5023), "the version that
introduced GpuMmu, with every later member structurally absent", and deferred any move to "a later decision with
its own reason". The reason has arrived from two sides:

- The owner's standing direction (2026-09-24) prefers bleeding-edge versions where practical and treats historical
  pins as temporary, to be removed with evidence.
- The graphics stack after M12 (ADR 0017: hosted ICD for the desktop, system D3D11 on DXVK, native D3D12 on
  vkd3d-proton) meets dxgkrnl features that arrived after WDDM 2.0: the later present, flip-queue and
  synchronisation DDIs, the caps and allocation-info shapes that the D3D12 runtime and modern DWM ask for.
  Discovering the version pin at that point would cost more than lifting it now.

The WDK in `toolchain/nuget` (10.0.26100) defines versions up to `DXGKDDI_INTERFACE_VERSION_WDDM3_2` (0x11007,
`shared/d3dukmdt.h`), and the header's default is 3.2. `driver/kmd/bc250kmd.h` already records how the version
changes structure shapes and how the display-only probe (`scratch/m7-stagea/probe.c`) measured which members
move; that method carries over.

## Decision

1. **Target: the newest `DXGKDDI_INTERFACE_VERSION_WDDM*` the WDK in `toolchain/nuget` defines**, 3.2 today, and
   the version the driver reports to dxgkrnl follows the version it is compiled at. When the WDK is updated, the
   target moves with it; a lower value is a temporary pin that needs the failing evidence and the work to remove
   it written down, as the toolchain rule says.
2. **One version for one binary**, as before: the macro shapes `DXGKRNL_INTERFACE` and the `DXGKARG_*` structures
   and our device structure carries one of them.
3. **The move is staged and measured, not flipped:** the size-and-offset probe is run at 2.0 and at the target for
   every structure the driver touches; every DDI the target version makes mandatory is implemented or given an
   honest failure return; every optional DDI stays absent until an experiment needs it; caps that the target
   version lets the driver claim (and that the runtime would start relying on) are claimed only with evidence.
   Each stage has an experiment with a fresh-boot control on unit A and the boot-loop guard behind it.
4. **Owner of the work:** the kernel driver's owner (the agent holding KMD changes and their validation at the
   time, Codex on 2026-09-27), coordinated with the user-mode side because the UMD/KMD private blob and the caps
   the runtime sees may change shape.

## Consequences

- `driver/kmd/bc250kmd.h`'s `C_ASSERT` on 2.0 becomes an assert on the target, and its comment moves from "why
  2.0 is safe for the display-only path" to "which members moved between 2.0 and the target and what the driver
  does about each".
- The contract matrix (`docs/wddm_contract_matrix.md`) gains the DDIs the target version adds; the ABI gates of
  E35 (facts M537/M538) are re-run at the target before any promotion.
- Windows on unit A (the lab's Windows 11 build) must actually offer the target version to a driver; if it caps
  lower, that cap is the measured, temporary pin of point 1.

## References

- ADR 0008 (point 2 superseded), ADR 0017, `bc250-win/CLAUDE.md` "Toolchain and Mesa freshness".
- WDK 10.0.26100 `shared/d3dukmdt.h` (version constants), `km/d3dkmddi.h`, `km/dispmprt.h`.
- `driver/kmd/bc250kmd.h` (version note), `scratch/m7-stagea/probe.c` (the offset probe, outside the repo).
