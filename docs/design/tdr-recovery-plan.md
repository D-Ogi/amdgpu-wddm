# TDR recovery: research brief and staged plan

Status: plan. Nothing in this page is measured unless it names a fact, an evidence directory or a
local defect record. It builds on [hang recovery](hang-recovery.md), which owns the soft engine
reset of nodes 0 and 2. This page does not repeat that design.

The goal is a GPU hang that ends in an engine reset or an adapter reset, after which the desktop
comes back. Today some hangs end in bugcheck 0x116 instead. The plan names the cheapest stage first
and gives each stage a positive control, a lab trial of less than three minutes and a pass criterion.

## Sources and revisions

| Source | Revision |
|---|---|
| Microsoft driver documentation, `ref/windows-driver-docs` | `110f60ea` (2026-09-15) |
| DDI reference, `ref/windows-driver-docs-ddi` | `7515063` (2026-07-21) |
| WDK headers as text, `ref/ddi-display` | WDK and SDK 10.0.26100 |
| Linux amdgpu, `driver/amdgpu-import/reference` and `ref/linux-src` | v6.18, `7d0a66e4bb90` |
| IGT, `ref/igt-amdgpu-tests` | `8d3284b5ae16` |
| HLK test pages, `ref/web-docs/hlk-graphics` | fetched 2026-09-26 |
| This repository | `origin/main` at `00c3a83f` |

The local defect log (BD numbers) and the lab scratch trees are outside this repository. This page
cites them by identifier only.

## 1. The contract

### 1.1 The engine path

`tdr-changes-in-windows-8.md:81-96` gives the sequence of a successful engine reset. The GPU
scheduler does these steps:

1. It sends a preemption request.
2. It detects the timeout. `TdrDelay` is the time that the GPU can delay the preemption request
   (`tdr-registry-keys.md:53-62`).
3. It takes a snapshot of the last submitted fence and the last completed fence. It then ignores the
   interrupts of that engine.
4. If the hardware queue is empty, it stops here.
5. It flushes the queued DPCs.
6. It calls `DxgkDdiQueryDependentEngineGroup` and then `DxgkDdiResetEngine`.
7. It checks `LastAbortedFenceId`. A value below the last completed fence or above the last submitted
   fence is bugcheck 0x119 with parameter 1 = 0xA (`tdr-changes-in-windows-8.md:90-95`).
8. If the reset affected a paging packet, an adapter reset follows (`tdr-changes-in-windows-8.md:96`).

On success the scheduler puts the device of the aborted packet into the error state. It gives the
render packets behind it new fence IDs and submits them again. It submits paging packets again with
their original fence IDs, in their original order (`tdr-changes-in-windows-8.md:61-66`).

`DxgkDdiResetEngine` returns only when three conditions hold (`nc-d3dkmddi-dxgkddi_resetengine.md`,
Remarks):

- the reset operation is complete.
- nothing remains in the hardware queue of the physical adapter.
- the node can accept new packets.

`DXGKARG_RESETENGINE` has three members in the WDK 10.0.26100 header: `NodeOrdinal`,
`EngineOrdinal` and `LastAbortedFenceId` (`ns-d3dkmddi-_dxgkarg_resetengine.md`). The conceptual
page names a `LastCompletedFenceId` member. The header wins, as [hang recovery](hang-recovery.md)
already records.

A special case is legal. When the queue is empty from the point of view of the driver, the driver
names its last completed fence as aborted (`tdr-changes-in-windows-8.md:98-100`). Our verdict 7 is
that case.

### 1.2 A failure return

A failure from `DxgkDdiResetEngine` is the documented answer of hardware that "is in an invalid
state" or "is incapable of resetting the nodes". The scheduler then does an adapter reset and
restart, with the behaviour from before Windows 8 (`tdr-changes-in-windows-8.md:102-107`). The
debugger then reports `TdrReason` 9, `TdrEngineTimeoutPromotedToAdapterReset`
(`tdr-changes-in-windows-8.md:109`).

A failure from `DxgkDdiResetFromTimeout` is a bugcheck ("otherwise, the operating system bug checks
and causes a restart", `nc-d3dkmddi-dxgkddi_resetfromtimeout.md`, Returns). That bugcheck is 0x116.
Its parameter 3 is the error code of the last failed operation (`bug-check-0x116---video-tdr-failure.md`).
In every 0x116 of the local defect log, parameter 3 is 0xC0000001. That is `STATUS_UNSUCCESSFUL`,
the value that `Bc250WddmResetFromTimeout` returns.

### 1.3 The adapter path

The adapter path has four parts:

1. `DxgkDdiResetFromTimeout` resets the GPU. When it returns, the GPU must not read or write any
   memory (`nc-d3dkmddi-dxgkddi_resetfromtimeout.md`, Description). The overview page also says that
   the driver must stop memory access and should not access the hardware
   (`timeout-detection-and-recovery.md:47-49`).
2. No other thread runs in the driver during this call. The two runtime power DDIs are the
   exception. Interrupts and DPCs can still run (`thread-synchronization-and-tdr.md:19-24`).
3. After the return, VidMm purges the allocations. It calls `DxgkDdiBuildPagingBuffer` with
   `DXGK_OPERATION_TRANSFER` and `Transfer.Size` 0 for an eviction, and with
   `DXGK_OPERATION_UNMAP_APERTURE_SEGMENT` for an aperture. The driver should not access the GPU in
   these calls (`thread-synchronization-and-tdr.md:26-36`).
4. `DxgkDdiRestartFromTimeout` tells the driver that it can access the GPU again
   (`thread-synchronization-and-tdr.md:38`, `nc-d3dkmddi-dxgkddi_restartfromtimeout.md`).

For an adapter reset, the last completed fence of every node advances to the last submitted fence at
the time of the reset (`tdr-changes-in-windows-8.md:79`). The display is not named in this sequence.
The desktop comes back through a redraw. A WDDM 1.2 driver can do this without a visible flash
(`timeout-detection-and-recovery.md:53-57`).

The third-level synchronization page lists both timeout DDIs. It also says that "the graphics
hardware is idle" at that level (`threading-and-synchronization-third-level.md`). For a timeout,
that statement cannot hold for the engines. We read only the thread guarantee from it.

### 1.4 The other DDIs

| DDI | Contract | Source |
|---|---|---|
| `DxgkDdiQueryDependentEngineGroup` | Mandatory with `SupportPerEngineTDR`. Gives the nodes that a reset of one node also resets | `tdr-changes-in-windows-8.md:23-31` |
| `DxgkDdiQueryEngineStatus` | Mandatory with `SupportPerEngineTDR`. "Should always succeed". First-level synchronization | `nc-d3dkmddi-dxgkddi_queryenginestatus.md` |
| `DxgkDdiCollectDbgInfo` | Level zero for a TDR reason. Pageable, `PASSIVE_LEVEL` for 0x117 and 0x141 | `nc-d3dkmddi-dxgkddi_collectdbginfo.md`, `threading-and-synchronization-zero-level.md:23` |
| `DxgkDdiCancelCommand` | Called only with `CancelCommandAware`. An error return is bugcheck 0x119, parameter 1 = 0x9 | `nc-d3dkmddi-dxgkddi_cancelcommand.md` |
| `DxgkDdiResetHwEngine` | Hardware scheduling only. It reports the invalid contexts through `DxgkCbInvalidateHwContext` | `nc-d3dkmddi-dxgkddi_resethwengine.md` |

### 1.5 Registry values

`TdrLevel` 3 is the default, recovery on timeout. `TdrDelay` is the threshold in seconds, 2 by
default. The GUI of this driver sets it to 10. `TdrDdiDelay` (5 s by default) bounds how long
threads take to leave the driver before 0x116. `TdrLimitCount` (5) and `TdrLimitTime` (60 s) bound
repeated recoveries. `TdrDebugMode` 2 recovers without a debugger break. `TdrTestMode` is
"Reserved. Don't use." (`tdr-registry-keys.md:33-124`). The overview adds that engine timeouts
(0x141) do not count to the hang limit, and that the OS logs 0x142 when it blocks a process
(`timeout-detection-and-recovery.md:37-39`).

### 1.6 Open contract questions

- **Duration.** No page gives a time limit for `DxgkDdiResetFromTimeout` itself. Measure it in
  stage S1.
- **Scanout.** The display controller reads the primary surface while the engines are reset. No page
  says that this read breaks the rule of section 1.3. Record what dxgkrnl does to the display in
  stage S1.
- **The calls in between.** Which paging operations VidMm sends between the two timeout DDIs on our
  GpuMmu driver is not known. Log every DDI in that interval in stage S1.

## 2. Our KMD today

### 2.1 What each DDI does

| DDI | Behaviour at `00c3a83f` | Source |
|---|---|---|
| `DxgkDdiResetEngine` | Nodes 0 and 2: admission, then the soft recovery of [hang recovery](hang-recovery.md) with verdicts 1, 5 and 7 as success. Node 1 and every other verdict: `STATUS_NOT_SUPPORTED` | `driver/kmd/wddm.c:8002-8234` |
| `DxgkDdiResetFromTimeout` | Closes start health, the GFX, paging and compute gates. Returns `STATUS_UNSUCCESSFUL` every time | `driver/kmd/wddm.c:7897-7911` |
| `DxgkDdiRestartFromTimeout` | Closes start health, logs, returns `STATUS_SUCCESS` | `driver/kmd/wddm.c:7913-7920` |
| `DxgkDdiQueryDependentEngineGroup` | Each node depends only on itself | `driver/kmd/wddm.c:7925-7941` |
| `DxgkDdiQueryEngineStatus` | `Responsive` is the inverse of `WatchdogFaulted[node]` | `driver/kmd/wddm.c:7943-7962` |
| `DxgkDdiCollectDbgInfo` | Zeroes the buffer, takes no lock, reads no state | `driver/kmd/wddm.c:8243-8251` |
| `DxgkDdiCancelCommand` | Not set. `CancelCommandAware` is 0 | `driver/kmd/wddm.c:10991` |
| `DxgkDdiResetHwEngine` | Not set. The driver does no hardware scheduling | DDI table, `driver/kmd/wddm.c:10964-10970` |

The driver declares `SupportPerEngineTDR` (`driver/kmd/wddm.c:4819`) and preemption at the DMA buffer
boundary for graphics and compute (`driver/kmd/wddm.c:4901-4902`).

### 2.2 Where recovery failed

| Record | Node | What happened | Status |
|---|---|---|---|
| BD-114 | 0 | Verdict 3, nothing on the ring. `ResetEngine` refused, then `ResetFromTimeout` failed. 0x116 | Fixed by the watchdog rework and verdict 7 |
| BD-158 | unknown | Field report of 0x116. No minidump arrived. Not reproduced at 8 GiB | Open. The load stall was shader compilation, not a GPU hang |
| BD-164 | unknown | Field report of 0x116 at logon | Open, cause unknown |
| BD-188 | 1 | Paging hang after a resume to D0. `ResetEngine` refused twice, `ResetFromTimeout` failed. 0x116 | A later resume worked (PSP 11 of 11) |
| BD-209 | unknown | Two 0x116 on the GDI route. The minidump has no engine data. The recovery record was not read | Open |
| BD-235 | 1 | SDMA0 paging stall when a packet crosses ring byte 2^32. `ResetEngine` refused twice. 0x116 | Fixed by ring padding. The reseed arm hung unit A |

The open 0x116 cases fall into three classes:

1. **Paging hangs on node 1.** The contract promotes a reset that touches a paging packet to an
   adapter reset (section 1.1, step 8). An engine reset of node 1 alone cannot prevent 0x116. Only a
   successful `DxgkDdiResetFromTimeout` can.
2. **Node 0 or node 2 hangs that the wave kill cannot drain.** Examples are a command processor wait
   with no waves, and an admission refusal (verdict 8). The refusal goes to the adapter path, which
   always fails.
3. **Field reports without a class.** The recovery record and the log ring in their dumps were not
   read. Stage S0 reads them first.

### 2.3 Findings in passing

- `driver/shim/bc250_gfx.c:2830` says that `GRBM_SOFT_RESET.SOFT_RESET_CPC` exists only from gfx11 on.
  The gfx10.1 field header in `ref/linux-src` defines `SOFT_RESET_CPC`, `SOFT_RESET_CPF` and
  `SOFT_RESET_CPG` for `GRBM_SOFT_RESET`. The comment is wrong for this part. This page does not change
  the code.
- `bc250_sdma_reset_retained_instance` (`driver/shim/bc250_sdma.c:818`) has no caller.
- The KMD maps the GFX queue through the KIQ at bring-up (`driver/shim/bc250_gfx.c:1380-1466`). No
  file in `driver/kmd` uses the KIQ after bring-up.

## 3. Reset mechanisms on gfx1013

The node map is from `driver/kmd/gfx.c`. Node 0 is the GFX ring, mapped through the KIQ as a kernel
graphics queue (`async_gfx_ring` 1, `driver/amdgpu-import/reference/amdgpu_drv.c:222`). Node 1 is the
paging ring on SDMA0 (`driver/kmd/gfx.c:120`, `driver/kmd/gfx.c:826`). Node 2 is compute ring 1
(`driver/kmd/gfx.c:114-116`).

| Mechanism | Scope | Status on unit A | Source |
|---|---|---|---|
| Wave kill by `mmSQ_CMD` broadcast | Waves of one VMID | Measured on Windows | M835, M836, BD-102 (45 kills), BD-119 |
| Compute queue reset, `UNMAP_QUEUES` with `RESET_QUEUES` through the KIQ | One compute queue | Source only | `gfx_v10_0.c:9578-9646`, `gfx_v10_0.c:3766-3791` |
| GFX queue reset by `mmCP_VMID_RESET` through the KIQ | One GFX queue | Source only | `gfx_v10_0.c:9511-9576`, `gfx_v10_0.c:3821-3870` |
| SDMA queue reset (stop, reset, restore) | One SDMA instance | Measured on Linux for SDMA1 only | M388, M389, `sdma_v5_0.c:1543-1632` |
| SDMA instance reset in the shim | One SDMA instance | Extracted, used for reload only | M372, M373, `bc250_sdma.c:657-811` |
| `mmGRBM_SOFT_RESET` of the CP and GFX blocks | Whole GFX block | Source only | `gfx_v10_0.c:7608` and after |
| RLC safe mode | Not a reset | No effect with `cg_flags` 0 | [hang recovery](hang-recovery.md) |
| Mode 1 reset through the PSP | Whole ASIC | Absent | M53, M351 |
| Mode 2 reset | Whole GC | No SMU callback for this part | `cyan_skillfish_ppt.c:69` |
| BACO | Whole ASIC | Not supported | `nv.c:442-472` |
| Retained power cycle in our KMD (D3 and D0) | Engines, IH, PSP, GART | Observed once (2026-10-20, BD-188 notes) | `driver/kmd/power.c:43-118` |
| PnP stop and start in one boot | Whole driver | Measured | M432 |

Notes on the table:

- Linux v6.18 sets the per-queue reset flag for gfx10 when SR-IOV is off
  (`gfx_v10_0.c:4955-4962`). For SDMA 5.0 it needs the IP versions 5.0.0, 5.0.2 or 5.0.5 and firmware 35
  or later (`sdma_v5_0.c:1425-1437`). Whether this unit meets the SDMA condition is not measured.
- `nv.c:442-472` sends MP1 11.0.8 to BACO or mode 1. Neither works on this part, so Linux has no
  adapter reset here. That agrees with M53, where `amdgpu_gpu_recover` hangs unit A.
- The SMU header `smu_v11_8_ppsmc.h:61` declares a message for a GC soft reset. amdgpu never maps or
  sends it. It is outside our SMU allow-list and its effect is unknown. This plan does not use it.
- Fact M59: the SDMA ring pointers cannot be written back to zero. The BD-235 reseed arm did that on an
  engine that had run, and it hung unit A. No stage of this plan writes ring pointers to zero.

The retained power cycle is the only whole-engine re-initialization that our KMD has. It brings the
engines down and up without a PnP restart (`driver/kmd/power.c:60-107`). Its resume path also
restores the display (`driver/kmd/power.c:6-41`). A timeout reset must keep the display as it is, so
stage S3 uses the engine steps only.

## 4. Tests

### 4.1 Microsoft tools

- **`D3DKMTEscape` with `D3DKMT_ESCAPE_TDRDBGCTRL`.** The escape takes a `D3DKMT_TDRDBGCTRLTYPE`
  value. `D3DKMT_TDRDBGCTRLTYPE_FORCETDR` (0) forces an adapter TDR. `D3DKMT_TDRDBGCTRLTYPE_ENGINETDR`
  (8) forces an engine TDR on the node in `D3DKMT_TDRDBGCTRL_ESCAPE` (`ref/ddi-display/d3dkmthk.md`,
  lines 33906-33939 and 41099-41112). The page says that the escape is for testing only. Whether
  dxgkrnl accepts it on a retail system without debug settings is not known. Stage S0 measures it.
- **`dxcap -forcetdr`.** The Graphics Tools option forces a TDR. No local copy of its page was read
  for this plan. Use it as a second generator when the escape client fails.
- **Registry.** `TdrDebugMode` 2 (recover without a break) is the lab setting for these trials.
  `TdrTestMode` is reserved, so the plan does not use it (`tdr-registry-keys.md:33-124`).
- **Driver Verifier.** The display miniport options check the DDI rules. Run them on a later stage,
  after the recovery path works, because a verifier stop in that path is again a bugcheck.
- **HLK.** Row R1 of `docs/testing/hlk-certification.md:187` keeps hang recovery pending. The TDR
  jobs are in `docs/testing/hlk-certification.md:78` and in `ref/web-docs/hlk-graphics/INDEX.md`.
  They include "TDR2 - Multiple TDR Compute Lockout and Recovery Test (Enumerated Adapter)" and
  "Standard Two Device Test Graphics (Uninterrupted when another app TDRs)". The HLK runs are a separate
  work lane (docs/testing/hlk-certification.md). This plan only names the jobs that stage S5 makes possible.

### 4.2 Hang generators

| Generator | Node | Kind | Status |
|---|---|---|---|
| `tools/win/hangclient`, `bc250hang --lab --long` | 2 | Shader loop | Exists. Rebuild it, the 2026-10-21 cleanup deleted the binary |
| `bc250hang --lab --short` | 2 | Positive control, no hang | Exists. Expected `RESULT=SHORT-COMPLETED-DEVICE-ALIVE` |
| Quake II RTX ray tracing hang (BD-102) | 0 | Real application | Fixed. Usable only with the old build |
| DispatchMesh hang (BD-119) | 0 | Real application | Fixed. Usable only with the old build |
| Lab paging seed at the 2^32 crossing, padding off | 1 | SDMA0 stall | Measured 3 of 3 in BD-235. `driver/kmd/guard.c:275-309`, lab flavour only |
| TDR escape client (new, S0) | any | Forced TDR, no hang | To write |
| Vulkan hang client (new, S0) | 0 and 2 | Loop, wait on a host event, access to an unmapped address | To write |

IGT gives the pattern for the new client. `tests/amdgpu/amd_deadlock.c` has the subtests
`amdgpu-deadlock-gfx`, `amdgpu-deadlock-compute` and `amdgpu-deadlock-sdma`. Its helper
`amdgpu_hang_ring_helper` (`lib/amdgpu/amd_deadlock_helpers.c:437`) puts a `WAIT_REG_MEM` packet on the
ring that never completes. The Windows client can do the same from Vulkan with a wait on an event that
the host never sets, and then set it as the release control. `tests/amdgpu/amd_queue_reset.c` is the
pattern for a per-queue check.

### 4.3 Linux reference

Linux on the diagnostic USB is the reference for each queue reset. Two rows go to
`docs/linux-session-wishlist.md`:

- **L47.** A per-queue reset of the GFX and compute queues on unit A. Use the IGT deadlock tests with
  a short `lockup_timeout`. Trace `gfx_v10_0_reset_kgq` and `gfx_v10_0_reset_kcq`, and read
  `mmCP_VMID_RESET`, `mmCP_HQD_ACTIVE` and `mmGRBM_STATUS` before and after. A new context must then
  render a known pattern, as in E29.
- **L48.** An SDMA0 instance reset. E29 measured SDMA1 only (M388). Add a copy with a known pattern
  after the reset as the control.

Plan the plug for both rows. A failed queue reset makes amdgpu try a full reset, and M53 says that a
full reset hangs unit A.

## 5. Plan

### 5.1 Rules for every trial

- **Bound.** Each trial ends within 180 s of its start.
- **Kill criteria.** Stop the trial when SSH does not answer 90 s after the trigger, or when Tctl is
  87 C or more. Stop it also when the owner sets the STOP flag of the overlay.
- **Recovery.** Use the emergency channel first. Cut AC with the smart plug only when the lab is hung.
  Wait for the boot with `target.py boot --os windows --after-now --bound <s>` in the background.
  Copy the dumps and the guard log before any other step. After an unclean end, set `DpmMode` 1, open
  the GPU path again and restart.
- **Pass.** All of these must be true:
  - event 4101 is logged, with no event 1001 for 0x116.
  - the desktop is back (overlay screenshot or camera).
  - the completed fence of each node moves after the recovery.
  - a new D3D12 device works: `bc250hang --lab --short` gives `RESULT=SHORT-COMPLETED-DEVICE-ALIVE`.
  - the recovery counters of the guard log are balanced.

### 5.2 Stages

| Stage | Hypothesis | Code outline | Positive control | Trial | Pass | Time |
|---|---|---|---|---|---|---|
| S0 | We can force each TDR path on demand and classify the old 0x116 dumps | Tools only. A TDR escape client. A Vulkan hang client. `DxgkDdiCollectDbgInfo` copies a snapshot made earlier. A log line for each DDI between the two timeout DDIs | `bc250hang --lab --short` | T0 and T1 (below) | Each generator reaches the DDI that it targets | 2 days |
| S1 | An adapter reset with all rings idle can succeed | `ResetFromTimeout` returns success only when witnesses prove idle: read pointer equals write pointer, fences retired, `mmGRBM_STATUS` and `mmCP_STAT` idle, SDMA idle. It advances each node fence to the last submitted. `RestartFromTimeout` reopens the gates | FORCETDR on the current KMD gives 0x116 | FORCETDR on an idle desktop | Section 5.1 | 1 day code, 2 trials |
| S2 | A queue reset recovers nodes 2 and 0 after verdict 2 | Compute queue reset first, then the GFX queue through `mmCP_VMID_RESET`. Uses the KIQ after bring-up and sets up the queue descriptor again with the bring-up code. Lab selector `HangRecoveryMode` 2 | L47 on Linux, then verdict 2 on the current KMD | `bc250hang --lab --long` and the event wait client | Section 5.1 | 3 days, 4 trials |
| S3 | An adapter reset recovers a paging hang on node 1 | `ResetFromTimeout` stops the engines with the SDMA instance reset and the engine steps of the retained power cycle. The display stays as it is. `RestartFromTimeout` brings the engines up again | L48 on Linux, then BD-235 0x116 with the seed | Lab seed at the 2^32 crossing, padding off | Section 5.1 | 4 days, 4 trials |
| S4 | A wave that accesses an unmapped address can be killed and recovered | The wave kill for the memory fault case, after L43 | L43 on Linux | Vulkan client, unmapped address | Section 5.1 | 2 days |
| S5 | Recovery holds for repeats and for the HLK jobs | No new path. Counters and limits only | S1 to S3 pass | Five TDRs in 60 s, then the HLK TDR jobs | `TdrLimitCount` gives 0x142 as specified, no 0x116 | 1 day, HLK lane |

S1 has a smaller scope than it seems. It recovers only the case where the hang already ended, or where
no engine has work. That case still causes 0x116 today, because the adapter path always fails. S1
must never return success while a witness reads a busy engine. In that case it returns the old
failure.

S3 is the stage with the most risk. The retained power cycle was observed only once at D0. Before the
first hang trial, a lab-only escape must run the engine steps once on an idle GPU. After it, the
desktop and a new device must work.

### 5.3 The first trial: T0

T0 measures the stage-1 baseline on the installed lab KMD. It changes no code.

1. Build the hang client:
   `pwsh -NoProfile -File tools\win\hangclient\build.ps1 -Kits <toolchain>\nuget -Out <scratch dir>`.
2. Copy the old kit `hang-lab.ps1` into a new kit directory. Pin it to the installed KMD and Vulkan
   driver. Do not edit the old kit.
3. Run `Record -Save`, then `Run short`, then `Run long`, then `Desktop`, then `Record -Compare`.
4. Pass: the short run gives `RESULT=SHORT-COMPLETED-DEVICE-ALIVE`, the long run gives
   `RESULT=HANG-DETECTED-DEVICE-REMOVED`, the desktop is back and there is no 0x116.

T1 follows T0 in the same session. It runs the TDR escape client with ENGINETDR on node 0 and an idle
GPU. The prediction is verdict 7 and a recovery. When dxgkrnl refuses the escape, T1 records that and
the plan uses `dxcap -forcetdr` instead.

### 5.4 Stages for tester.34

S0 and S1 can join tester.34. S0 adds tools and log lines only. S1 changes the failure policy for the
idle case only, and a failed witness keeps the old behaviour. S2 and S3 need the Linux rows and new
lab selectors first, so they come later.

## 6. Register lookups

All names come from `python tools/regcalc/regcalc.py lookup` at `00c3a83f`, run on 2026-10-28. Each
name was found. The field names come from the gfx10.1 field header in `ref/linux-src`.

`mmSQ_CMD`, `mmCP_VMID_RESET`, `mmCP_HQD_DEQUEUE_REQUEST`, `mmCP_HQD_ACTIVE`, `mmCP_GFX_HQD_ACTIVE`,
`mmCP_GFX_HQD_DEQUEUE_REQUEST`, `mmSPI_COMPUTE_QUEUE_RESET`, `mmGRBM_SOFT_RESET`, `mmGRBM_STATUS`,
`mmGRBM_STATUS2`, `mmGRBM_GFX_INDEX`, `mmRLC_SAFE_MODE`, `mmRLC_CNTL`, `mmCP_ME_CNTL`, `mmCP_MEC_CNTL`,
`mmCP_STAT`, `mmCP_RB0_RPTR`, `mmCP_HQD_PQ_RPTR`, `mmGCVM_L2_PROTECTION_FAULT_STATUS`, `mmSDMA0_FREEZE`,
`mmSDMA0_F32_CNTL`, `mmSDMA0_CNTL`, `mmSDMA0_STATUS1_REG`, `mmSDMA0_GFX_PREEMPT`, `mmSDMA0_GFX_RB_CNTL`,
`mmSDMA1_FREEZE`.

## 7. Decisions for the lead

1. Accept S1 for tester.34. It changes the policy that `ResetFromTimeout` always fails.
2. Permit one planned 0x116 on unit A as the positive control of S1.
3. Plan one Linux session for L47 and L48, with the plug ready.
4. Accept the lab-only selector values for S2 and S3.