# BD-028: host geometry assumptions removed

2026-09-24. Source-only work by Codex memory sub-agent. No production driver edits,
lab access, firmware changes, deployment or commit. Existing-ID status stays
TRIAGED: the host/test subtask is complete, the carve-out/residency epic is not.

## Changes

Five files were changed, all within delegated ownership:

- `driver/kmd/test/paging_mc_test.c:25-68`: supplied geometry drives the same
  conversion/round-trip controls for synthetic 8, 12 and 16 GiB windows. Both
  origins change between cases. Positive controls cover the last byte and a
  full page above the former 8 GiB boundary, plus the retained range checks.
- `driver/kmd/test/dcn_translate_test.c:12-69`: retains the old 256 MiB fixture
  and adds 8/12/16 GiB cases with different origins, a complete surface ending
  at the window boundary and a surface above 8 GiB. These are arithmetic
  fixtures, not predicted BIOS layouts.
- `tools/wddm_contract_check/host/qai_bridge.h:59-70`: explicit `bc250h_geometry`
  handoff containing size, CPU physical origin, GPU MC origin and POST geometry.
- `tools/wddm_contract_check/host/qai_bridge.c:27-99`: `bc250h_start_geometry`
  populates the adapter from supplied values. The original `bc250h_start` remains
  as the unchanged-geometry 8 GiB convenience entry. `bc250h_fixture_geometry`
  labels named synthetic 8/12/16 GiB inputs; no device query is fabricated.
- `tools/wddm_contract_check/host/qai_test.c:789-882`: `--vram-gib 8|12|16`
  selects the fixture for both enabled and disabled VRAM cases; default is 8.
  Output prints size and both origins and explicitly says there is no hardware
  discovery. The API can also accept independently supplied geometry, not only
  these three fixtures.

The literal historical addresses remain only as explicit fixtures. They no
longer silently determine every host adapter's geometry. Sixteen GiB is a host
arithmetic control, not a claim that this 16 GiB physical board can reserve all
its RAM for VRAM and still boot Windows.

## Baseline drift kept separate

Before edits, the full old QAI harness failed to compile:

- `run.ps1` lacks the shim include path needed by current `wddm.c` for
  `bc250_gfx.h`.
- Bridge stubs declared `VidMmStart` and `VidMmUpdatePageTable` as `void`, while
  the current header requires `NTSTATUS`.

Baseline output: `qai-baseline.log`. The two bridge stubs were updated to the
current return type and explicit success (still stubs, not the real memory
implementation). No production requirements were removed. The full harness
runner and its remaining historical assumptions were not revived under this
bounded task. Therefore there is no claim that the entire current
QueryAdapterInfo/context/paging/submit suite passes, or that its complete CLI
binary was run. Separate bridge and caller compilation does pass.

## Validation

| Control | Result | Output |
|---|---|---|
| Actual `paging_mc.c` and `dcn_translate.c`, parameterized paging test | 63 checks, 0 failed; kernel-flags compile passes | `paging.log` |
| Actual `dcn_translate.c`, retained and new geometry tests | 78 checks, 0 failed; kernel-flags compile passes | `dcn.log` |
| Actual bridge initializer extracted verbatim and compiled against current WDK/device declaration | 87 checks, 0 failed | `geometry-controls.log` |
| Whole edited `qai_bridge.c` with WDK includes; whole edited `qai_test.c` with user includes | `/W4 /WX` compilation passes | `geometry-controls.log` |
| Scratch-only mutation imposing an 8 GiB maximum on both conversion functions | Paging: 12 failures; DCN: 14 failures; both exit 1 | `paging-negative.log`, `dcn-negative.log` |
| Diff whitespace | `git diff --check` passes on the five owned files | command output |

The bridge control captures the BC250_DEVICE passed to a stubbed WddmStart.
It checks all supplied address/size/POST fields for 8/12/16 GiB, both VRAM gate
states, the legacy 8 GiB default, and a separate 10 GiB/1680x1050 geometry supplied
through the public struct. It proves the initializer handoff only; production
WddmStart, segment layout, memory ownership and GPU activity are not simulated.

Generated controls and reproducible scripts are alongside this report:
`generate_geometry_controls.py`, `geometry_actual.c`, `geometry_main.c`,
`run_geometry_controls.ps1`. All outputs are under `scratch/build/bd028`.
Negative sources are under `negative/`; production conversion functions were
never modified.

Frozen source: `source/SHA256SUMS.txt`. It covers the five changed files plus
unchanged conversion functions/headers, current device header and test runners.
Snapshot creation script: `freeze.py`. Files in the shared production tree were
not copied back from this snapshot.

## Remaining BD-028 gates

The earlier M435 geometry consistency work is separate and was not changed.
The production conversion functions already take geometry arguments; this task
fixes limited test coverage and the host bridge's assumed input geometry.

`docs/research/bios-analysis-followup.md:36-57` records the important limits:

- Static Setup/AGESA analysis shows a 12G option and a consuming code path, not
  successful memory training, Windows boot or GPU access on unit A.
- Firmware writes still require explicit consent and a programmer-made recovery
  backup. L35 software SPI capture is read-only and is not that backup. The
  previously captured AmdSetup and queued SPI work should be tracked from STATE,
  not inferred from these host fixtures.
- A 12 GiB carve-out currently projects to **12,440,829,952 bytes = 11.58642578
  GiB** of application segment capacity for the recorded framebuffer geometry.
  Page tables, POST framebuffer and private reservations consume the remainder.
  Other live allocations reduce model headroom further. The full 12 GiB model
  residency goal therefore is not achieved merely by selecting a 12G carve-out.
- NVMe pagefile or GTT increases do not establish local GPU residency. Future
  acceptance needs actual capacity discovery and complete-set residency/content
  controls with every tested allocation kept alive. No such trial ran here.

## Proposed existing-ID comment

- 2026-09-24 Codex: BD-028 host/test subtask completed in source, with frozen
  inputs and logs at scratch/m9/bd028. MC/DCN conversions pass63/78 checks on
  synthetic 8/12/16GiB and varied bases; an injected8GiB clamp is detected.
  QAI bridge now takes explicit geometry, retains the historical8GiB default,
  and exposes named synthetic fixtures through --vram-gib. The actual initializer
  handoff passes87 checks; whole bridge/caller compile. Full old QAI runner has
  pre-existing include/stub drift and is not claimed passing. No production or
  firmware change. Keep TRIAGED: physical12G training and the11.58642578GiB
  application-capacity limit remain distinct open gates.
