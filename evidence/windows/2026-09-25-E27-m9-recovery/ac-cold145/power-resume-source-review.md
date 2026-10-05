# Adapter power resume review after candidate 145

Read-only source review, 2026-09-25. No source changes, lab calls or power transitions.

The next implementation needs a retained-owner hardware suspend/resume path. Calling
Bc250StopDevice/Bc250StartDevice, WddmStop/WddmStart, or the current startup coordinator
from SetPowerState is invalid with live OS objects. Merely reopening SMU also does not
restore translation, firmware, queues, interrupts or display.

## Microsoft contract (local sources)

Reference DDI snapshot: MicrosoftDocs/windows-driver-docs-ddi
7515063cea4c9e98db6a92986c5b4ddb0463fd16. Conceptual snapshot:
MicrosoftDocs/windows-driver-docs staging 110f60eaf2ac5836e644d320c1e92c1011f2af5e.
Paths below are relative to workspace P:/bc-250.

1. `ref/windows-driver-docs-ddi/wdk-ddi-src/content/dispmprt/nc-dispmprt-dxgkddi_set_power_state.md`:
   line26 requires PASSIVE_LEVEL; lines59-69 distinguish adapter from child and pass
   D1/D2/D3 plus transition reason. Line78 requires saving context on power-down and
   restoring it on D0. Line84 forbids relying on ActionType during D0: retain the
   previous down transition in the adapter. Line80 says a VGA-enabled hibernating
   adapter must keep display available and let the bus power it down. Line82 requires
   tolerating a disconnected child. Line74 says this DDI should not fail; returning
   success with dead engines is not a supported positive path or an error strategy.
2. Same file line86: D0 should call DxgkCbAcquirePostDisplayOwnership. If successful,
   assess returned mode; otherwise do not assume a mode remains, initialize display.
   Do not overwrite cached pitch/geometry underneath live allocations without a
   coherent VidPn/display transition.
3. `ref/windows-driver-docs/windows-driver-docs-pr/display/threading-and-synchronization-third-level.md:15-18,32,40`:
   SetPowerState has Level Three: GPU idle, no scheduler DMA in flight and video memory
   evicted. QueryAdapterInfo is an explicit concurrent exception. These guarantees
   do not stop private timers, IH DPCs or NoAdapterSynchronization escapes by themselves.
4. Same conceptual directory, `plug-and-play--pnp--start-and-stop-cases.md:61`:
   D0 return must have source visibility FALSE, with synchronization maintained and
   hardware producing black pixels. OS later makes the first rendered frame visible.
   A retained old primary or software SourceVisible=FALSE alone is insufficient.
5. `.../display/system-paging-process.md:20-22,36-38`: paging page tables are evicted
   on power transition and reinitialized after content loss. Power-on UpdatePageTable
   is forced CPU_VIRTUAL and completed synchronously by CPU. Preserve the software
   process/context and mapping identities; accept the OS's new table contents rather
   than declaring retained VRAM contents valid or replaying stale OS PTEs as authority.
6. `ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/ns-d3dkmddi-_dxgk_segmentflags.md:102-122,210-225`:
   all preservation flags zero means purge on standby and hibernate. Current
   wddm.c:1807 intentionally leaves them zero. There is no basis to change those flags.
   Eviction covers OS segment contents, not a backup service for our private reserved
   firmware, ring, MQD, GART and scanout state.

D-states and system S-states are different. M56 records Linux firmware S0/S4/S5 and
s2idle, not a successful Windows GPU resume or a list of PCI D1/D2 support. Read the
actual Windows-supported system power states before choosing the acceptance action.
The historical M56 suggestion to keep the early-M7 stub is not the current M9 goal.
Root subsequently captured Windows powercfg /a before cold shutdown:
`scratch/m9/cold145/power-states.log`. Firmware offers no S1/S2/S3/S0 Low Power Idle;
hibernation is currently disabled, so Hybrid Sleep and Fast Startup are unavailable.
Thus the cold shutdown control is not a hybrid-resume test. After implementing resume,
S4 hibernation is the concrete candidate for a real power-loss positive control, subject
to enabling it and confirming Windows then reports it available. Do not schedule an
S3 test on this configuration. Device D1/D2/D3 callbacks remain a separate contract.

## Current implementation and concrete hazards

- `driver/kmd/pnp.c:321-335` closes StartHealth and SmuOwner only for adapter non-D0.
  It saves no requested state/reason, stops no WDDM timers or engines, and restores
  nothing at D0. Adapter and child routing must remain distinct.
- `wddm.c:1476-1556` WddmStart clears Device->Wddm and WddmAperture, creates a new object
  list, initializes VidMm and publishes a new scheduler owner. `wddm.c:1559-1650`
  WddmStop detaches/free its owner, zeros pending paging jobs, calls VidMmStop, and
  releases every OS process/device/context/allocation object still on the list.
  Power-down is not permission to destroy those handles or reset OS fence history.
- `startup.c:25-28,48-53` GpuStartupInitialize rejects Started/Wddm and live GART/PSP/IH.
  These are meaningful first-start preconditions. Clearing flags to bypass them would
  create a second owner without releasing the first. Its unwind also destroys owners.
- `gart.c:139-140,245-269,317` ENABLE reconstructs/zeros the AMD owner and GART table.
  `GfxPrepareStop -> HaltForMappingRetirement` (`gfx.c:514-552,3249`) retires live GTT
  mappings and performs the stop/reload policy, not a retained-resource suspend.
  GfxStop, GartStop and PspStop release state/backing. They cannot be the resume pair.
- `ih.c:362-380,595` IhStop/Fini closes ISR/DPC admission but then tears down the ring
  and releases its gpumem backing. Retain its close/join discipline; separate the
  hardware pause from teardown. Do not keep a DPC able to touch a powered-down BAR.
- `psp.c:62,113,127,280-319` prepared firmware currently reads files and is released
  after startup. A resume coordinator should retain required images before sleep
  rather than depend on file I/O availability during D0. Preserve provenance/version
  metadata; images remain driver-owned buffers, never evidence/repository files.
- `smu.c:110-143` is already a single serialized owner: stop joins complete mailbox
  transactions, marks offline, clears register pointer/version. D0 should start this
  same owner against valid BAR access, drain/query firmware, then run SmuPrepareClock
  before engine admission. No second bc250rd writer or raw mailbox escape.
- `wddm.c:2043-2054` UMDRIVERPRIVATE reads cached PSP/SMU metadata, but SMU stop currently
  makes the version unavailable. QueryAdapterInfo can run during power transitions:
  preserve a synchronized immutable capabilities snapshot while hardware is offline,
  and publish any resumed metadata update coherently, without an SMU query per request.
- `start_health.c` deliberately leaves Closed sticky after power-down. A verified
  resume needs an explicit reopen API for the completed power transition, a new
  observation epoch, and no inherited CONFIRMED flag. D0 arrival alone is insufficient.

## AMD reference ordering, not a measured BC-250 resume

PROVENANCE: Linux amdgpu sources in ref/linux-src at v6.18 commit
7d0a66e4bb9081d75c82ec4957c50034cb0ea449, GPL-2.0 repository; read-only reference,
no code copied in this task.

`drivers/gpu/drm/amd/amdgpu/amdgpu_device.c:3763,3806,5202-5244` separates suspend
from software teardown, ungates before IP suspend, suspends display separately,
evicts resources and stops fence activity before the remaining IP callbacks.
`amdgpu_device.c:4014-4135,5288` resumes COMMON/GMC/IH, loads firmware, resumes
non-display engines, initializes fences, then resumes display. Software owners persist.

`gfx_v10_0.c:7529-7578` uses hardware fini/init for suspend/resume; it does not call
software fini/init. MQD backup/restore branches at 7139-7188 distinguish suspended
state from a new allocation. `gmc_v10_0.c:1050-1067` disables/re-enables hardware and
resets VMIDs. `sdma_v5_0.c:1481-1502` disables context switching and SDMA, then resumes
hardware. `amdgpu_psp.c:3241-3339` stops TMR/ring and later restarts PSP and reloads
non-PSP firmware using retained software context.

Do not transplant newer S0ix/GFXOFF shortcuts into this part. Existing Windows
startup has measured deferred GFX TLB visibility until RLC runs; retain that
ordering constraint when splitting the coordinator. M53/M55 are failed reset/reload
controls, not successful suspend/resume evidence. Existing Linux wishlist L6 already
requests the missing post-wake GPU-function and register trace; no duplicate row needed.

## Bounded positive-path implementation plan

1. Add an adapter-owned power transaction/state (D0, suspending, suspended, resuming),
   retained last down D-state/ActionType, and a stable capabilities snapshot. Keep it
   separate from OS handle ownership. At PASSIVE, serialize transitions and telemetry;
   publish immutable data under a short lock, never hold a spin lock over waits.
2. Add WddmSuspendRetained/WddmResumeRetained. Close new submission/timer rearming,
   confirm both execution and paging accepted queues/fences are drained, then cancel
   and join private DPCs. Preserve Objects, contexts, aperture state, captures, OS
   last-submitted/completed fences and allocation identities. Do not set failures as
   a substitute for completion, silently drop accepted work, or replay a submitted IB.
   Level Three supplies the expected empty-work positive case; instrument that invariant.
3. Split hardware suspend from storage retirement in GFX/SDMA, PSP, GART and IH.
   Save/reconstruct driver-private VRAM contents from CPU-owned state: GART entries
   for retained pinned backing, ring/MQD/writeback descriptors and firmware images.
   Disable consumers before translation, join IRQ/DPC access, then close native SMU.
   Do not call GpuMemRetireGttMappings, VidMmStop or owner-free functions here.
   Keep hibernation scanout obligations and bus-owned physical power switching intact.
4. On D0, use retained state rather than ActionType. With BAR/power accessible, reopen
   native SMU and validate 1000MHz/820mV policy; rebuild translation and private backing
   visibility, restore/load PSP firmware, IH and engines using retained owners and
   source-derived hardware entry APIs. Preserve the existing RLC/TLB visibility rule.
   New private ring pointers/fence slots may be initialized only after old work is
   proven retired; OS fence values and handles must not reset or be falsely completed.
5. Let Windows reconstruct its paging tables through immediate CPU_VIRTUAL updates.
   Retain CPU bookkeeping and aperture identities but invalidate hardware VMID/TLB
   bindings so later submissions bind current roots. Never assume OS-local VRAM
   tables survived just because this is unified GDDR6. Separate rebuilding our private
   GART/ring mappings from Windows-owned page-table initialization.
6. Reacquire POST ownership, validate the inherited mode and restore display timing,
   scanout and hardware blanking. First supported implementation can use the validated
   same-mode firmware handoff, but absence/change of firmware mode requires real DCN
   initialization; current inherited-only display cannot claim the full fallback.
   Publish both-engine readiness, keep source blank until OS visibility TRUE, rearm
   requested notifications, then reopen work admission. Begin a fresh health epoch
   only after verified hardware readiness. Resume is not PnP StartDevice: do not
   reconsume startup gates or silently manufacture a new UnconfirmedStarts admission.
7. Host controls use retained real object/map identities and accepted fence values,
   poisoned VRAM/private hardware state across the simulated sleep, CPU_VIRTUAL
   paging restoration, same-mode display handoff and visibility sequencing. Assert
   no owner destruction, no duplicated completion and no MMIO by offline telemetry.
   Then run one Windows-supported real suspend/resume with an application and its
   allocations kept open: pre/post byte readback, compute reference, paging/VA work,
   same application handles, display/input and typed health. This is the positive
   acceptance path; cold reboot or new allocations after PnP would not test it.

## First concrete code change

Implement the retained WDDM quiesce/resume pair and a small power transaction coordinator
with actual-source host tests, without calling StopDevice or clearing Started to satisfy
GpuStartupInitialize. Make source extraction split the verified hardware init phases from
first-start allocation/destructive unwind. Add dedicated retained-owner entry points for
GART/GFX/PSP/IH; do not rename the existing destructive helpers and call them unchanged.

Wire SetPowerState to that coordinator only when its hardware restore and display handoff
positive path are implemented. The remaining exact hardware boundary is real power-loss
resume on this unit: Linux M56 did not test GPU function, and current Windows warm-PnP
controls do not answer it. Use the existing L6 plan if a Linux witness is needed; no reset
experiment or new SMU command is justified merely by this source review.
