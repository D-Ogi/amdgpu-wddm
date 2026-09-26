# WDDM startup and paging readiness

Current acceptance: [M9 acceptance status](../research/m9-acceptance-status.md), reviewed through M462. Historical results below do not prove current candidate acceptance.

Status: implementation plan, 2026-09-23. Source review:
[facts M232](../facts.md) and
[immutable source snapshots](../../evidence/windows/2026-09-23-E27-m9-inference/startup-contract-review/).
This covers a remaining M9 DMA contract requirement, not a new milestone.

## Required outcome

When the OS can call a supported paging operation or submit its commands, the
driver must already own the resources and initialized engines needed to execute
it. A later CLI RUN cannot be the readiness mechanism for production WDDM.
Display-only mode remains a separate startup path.

At the original M232 review, the manual sequence opened full WDDM before GART,
PSP, IH and GFX initialization. Shared automatic startup subsequently replaced
that dependency; M317 observes both engines ready before initial OS paging on
07102. The remaining acceptance gates below include repeated initialization and
resource lifetime, not a continued dependency on manual CLI RUN.

## Initialization and ownership

| Phase | Required work | Publication and failure obligation |
|---|---|---|
| Prepare | Identify VRAM; allocate subsystem state, CPU mappings, paging metadata and private staging; validate gates/resources. | No supported paging capability may depend on a missing resource. Retain explicit ownership for each successful allocation. |
| Firmware preparation | Use existing psp.c file loading/validation at PASSIVE_LEVEL before GartLock. | Fail before hardware writes when files or storage are unavailable. Reuse existing validation; do not introduce another firmware uploader. |
| GART | Derive geometry through AMD setup, initialize table, enable translation. | The M229 descriptor and future builders use the same captured geometry. No later destructive table reset may erase accepted OS mappings. |
| PSP and interrupts | Load firmware through existing PSP sequence, initialize IH and its ownership. | Preserve the actual ring/TMR/interrupt state reached even on partial failure. Do not infer quiescence from a software state flag. |
| Engines | Run required initialization and ring controls; establish paging ring, marker, stage and invalidation-engine ownership. | Publish readiness only after success. A failed engine start requires proven halt or retained/quarantined DMA resources. |
| WDDM operation admission | Publish adapter state and the supported capabilities/segments consistent with initialized resources. | Earliest paging calls use a working implementation; no empty success, infinite buffer retry or dependence on a later CLI command. |
| Stop/restart | Stop admission, join builders/submissions, stop hardware, then release or retain resources according to actual retirement. | Never unload PSP or free mapped pages merely because a timeout expired. Restore mappings only within a verified new device/engine generation. |

The exact PnP/interrupt callback ordering must be verified before enabling this
sequence automatically. Moving RUN inside StartDevice invalidates the current
failure-cleanup comment that no RUN has occurred; changing that comment alone is
not a fix.

## Successful-start confirmation

The remaining full-table monitor handshake is specified in
[successful-start confirmation](wddm-start-confirmation.md). It requires a fresh
device-start identity and completed presentation progress before checked durable
confirmation. This is an implementation contract, not current acceptance.

## Implementation work

1. Extract internal initialization entry points shared with diagnostic escapes.
   Avoid a second copy of GART/PSP/IH/GFX sequences. Preserve lock order and keep
   firmware file I/O outside the fast mutex.
2. Represent completed and partially completed startup phases explicitly, with
   a reverse ownership/unwind path. Inject failures at every phase in host tests.
   A transition test must inspect resource retention, not just returned status.
3. Integrate startup/admission with the WDDM device lifecycle, then verify earliest
   callback delivery. Queue resources must exist before first execution; any
   deferred work must retain ownership and completion obligations.
4. Remove the production dependency on post-start CLI initialization. Keep CLI
   staging for controlled diagnostics, with repeated RUN/ENABLE rejected while
   OS resources are live unless a verified recovery transition owns them.
5. Validate a cold start, repeated device start, partial startup failure and OS
   map/transfer/unmap lifetime on the lab. These gates remain incomplete until
   actual engine completions, intact mappings and cleanup are observed.

## Rejected shortcuts

A CPU bootstrap PTE write alone is insufficient: current GART ENABLE zeros the
whole table afterwards. Recording desired mappings for replay may help recovery
or physical-segment resolution, but it does not make an unavailable engine ready
and cannot justify reporting an unexecuted DMA command complete.

STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER means capacity exhaustion. It must not
be used as a readiness wait. Internal DEVICE_NOT_READY/INVALID_PARAMETER returns
in current builders expose unresolved cases; they are not a completed OS-facing
failure policy.

## Acceptance evidence still required

- Failure injection proves no early publication, double release or loss of
  partially initialized ownership; successful control reaches real admission.
- Live traces establish startup ordering and first OS paging/submission callbacks.
- Known-pattern map/transfer/unmap controls prove correct physical pages, cache
  visibility, real fence completion and preservation across every supported
  initialization transition.
- Hardware halt/recovery evidence supports freeing DMA storage. Failed recovery
  remains a failure; elapsed time and a DWM restart are not halt evidence.
- Full M9 still includes alias handling, restricted DDI returns, CPU aperture
  policy, OS lifetime, paging pressure and equivalent Windows/Linux performance.


## M233 implementation progress

Stop now propagates retirement failures into dependent PSP/GART teardown and
retains resources on uncertainty; a later stop cannot turn missing software state
into a successful retirement verdict. Same-object restart refuses after failure.
Extracted-function tests cover128 failure combinations and manual FINI.
This does not provide containment across device removal/recreation, a hardware
reset, or automatic startup. Those acceptance gates above remain open.


## M234 implementation progress

Shared Gart/Psp/Ih/GfxInitializeHardware entry points now exist. Diagnostic paths
use the same unchanged command implementations. Reports are caller-owned,
nonpaged and synchronous; native and partial-completion failures propagate.
They are not called automatically during StartDevice yet.

Next the coordinator must allocate reports and validate resources before hardware
writes, retain a phase/ownership record, and unwind partial starts using the stop
verdicts. Firmware preparation currently remains inside PSP execution; move its
preflight before hardware activation as part of that coordinator design.
Verify paging readiness independently of generic GFX stage completion before
publishing WDDM state.


## M235 implementation progress

PspPrepareFirmware now reads and validates the existing firmware files without
hardware writes. It returns an opaque owner of the exact buffers, which
PspInitializePrepared borrows without re-opening files. The coordinator must call
PspReleaseFirmware on every success/failure path after execution or unwind.
Preparation is available before GART enable; the coordinator itself is still
unimplemented. The existing diagnostic path continues to read/free its own inputs.

## M236 implementation progress

GpuStartupInitialize now coordinates GART, prepared PSP, IH and GFX. The caller
provides nonpaged report storage and exclusive ownership of an unpublished
device lifecycle. Preflight checks objects, gates, callbacks, MSI, cold GFX state
and captured aperture geometry. Attempted and completed phase masks differ on
failure. Hardware failure runs the dependency-aware stop chain and records its
quarantine verdict without replacing the initiating error. Firmware ownership
ends on every exit; detailed subsystem reports stay with the caller.

Final readiness checks both submission paths, system paging window, staging,
ring and fence resources, plus unchanged aperture geometry. These are structural
checks after existing stage controls, not proof of live OS paging. Engine resource
allocation still occurs inside the existing GFX sequence.

291 host checks pass, with mutation controls and firmware/stop regressions.
The coordinator is not called by WddmStart yet. Verify interrupt delivery and
callback ordering, then integrate under actual PnP lifecycle exclusion and reject
destructive diagnostic reinitialization while OS resources are live.

## M237 interrupt contract and preflight

The local Microsoft StartDevice contract explicitly requires enabling adapter
interrupts. SynchronizeExecution can fail when the interrupt is not connected;
the coordinator now verifies actual synchronized callback delivery before GART
planning or firmware preparation. A callback pointer alone is insufficient.
Native failures propagate, with no hardware activation fallback.

The ISR/DPC review found a separate publication hazard: IhExecute invokes
bc250_ih_hw_init, which enables the ring and interrupts, before copying DpcAdev
and setting Active. IhInterrupt ignores interrupts while Active is zero.
Lost notification in that interval is a hypothesis, not a measured failure.
Simply moving Active earlier would expose incompletely initialized ring state.

Next split hardware preparation from final enable, preserve upstream register
ordering, publish the prepared DPC context and perform the short final transition
under interrupt synchronization. Allocation and blocking cleanup must stay
outside that callback. Verify refusal/unwind and stop transitions before wiring
the coordinator to StartDevice. WDDM DPC paths already guard NULL Wddm, but
that alone does not prove concurrent lifetime correctness.

## M238 split IH sequence

bc250_ih_hw_prepare leaves delivery disabled; bc250_ih_hw_enable performs only
the final upstream read-modify-write. The compatibility hw_init wrapper calls
both. Replay confirms14+1 writes against the15-write E03 Linux sequence.

The KMD still uses the combined wrapper. Its publication hazard remains open.
Next publish the prepared DPC context and use a synchronized short final enable.
The callback needs a DIRQL-safe register backend: the ordinary sequence's
RecordFault may call GuardLog unless its Dpc flag is set. Keep allocation,
blocking cleanup and waits outside that callback; test native synchronization
failure, register failure, immediate interrupt and stop transitions.

## M239 IH publication integration

Real INIT now uses prepare, publishes the DPC copy/backend and resets consumer
state, then calls SynchronizeExecution for the short final-enable transition.
Active is set before the enable write under ISR exclusion; failures deactivate
it. A separate nonpaged EnableSequence and single-write slot keep caller escape
output and logging out of DIRQL. Report/fault metadata is copied after return.
Native synchronization failure, absent invocation and false results propagate;
IhExecute invokes existing Fini on failure. PLAN retains its no-write model.

95 extracted-code checks pass, including immediate modeled ISR delivery and
callback failures; a late-Active mutation fails7. WDK integration builds.
Existing Fini/Stop interrupt exclusion and late-DPC retirement need review and
tests before automatic startup. No lab interrupt delivery has been measured.

## M240 IH stop admission and retention

Fini now synchronizes Active closure with ISR before flushing DPCs. This joins
an ISR that read Active before closure and has not yet queued its DPC. Failed
synchronization closes software admission and drains but authorizes no hardware
teardown or release; the device becomes quarantined. A failed hardware halt also
retains the full ring description instead of clearing its owner state.

IhStop covers failed GartDevice lookup with admission closure and quarantine.
Diagnostic INIT/PLAN refuses quarantine; FINI/STATE stay available.177 host
controls pass, including a late ISR model; removing synchronization fails54.
Runtime interrupt disconnection, halt and remove/reload remain unverified.

Next integrate the startup coordinator before WDDM publication, including cleanup
of unpublished VidMm resources, and reject destructive diagnostics during OS
ownership. Host coverage is required for all publication/failure boundaries.

## M241 WDDM admission integration

WddmStart now prepares its state/report, validates GPU VA and memory layout,
captures the aperture, prepares VidMm, runs the coordinator and only then
publishes Device.Wddm atomically. Failed starts clean unpublished VidMm and
temporary CPU state; hardware ownership follows coordinator/PnP unwind. Full
WDDM no longer starts in the old diagnostic-only mode awaiting manual RUN.

Bc250Escape admits known observational commands and GFX/IH STATE under FullWddm.
Destructive initialization, teardown, PLAN, test submissions, raw writes and
diagnostic flips return DEVICE_BUSY. The guard spans FullWddm ownership; actual
PnP/escape concurrency remains a runtime validation item.

89 host controls and319 coordinator regressions pass; WDK builds. Not deployed.
Before lab, replace manual bring-up scripts and verify clock control completes
before GPU startup. Correction M242: the clock task uses separate bc250rd SMU
IOCTLs and is not blocked by the miniport escape policy. Cold-start ordering
remains a deployment prerequisite. Live callbacks, paging, halt/reentry and full M9 acceptance remain.

## M243 live clock control and first startup trial

The new clock-check command passed live positive/negative/recheck controls on
unit A:1000MHz/VID116, mismatch1001 rejected. This validates the query tool, not
clock ordering at boot. See the [first live trial plan](../../experiments/E27-m9-inference/startup-live-plan.md).
Before an automatic-start trial, add a tested one-shot lab gate so an unexpected
reboot cannot attempt full WDDM before underclock verification. This trial
mechanism does not close production cold-boot lifecycle requirements.

## M244 existing one-shot gate durability

EnableFullWddm1 already provides a one-shot gate in DriverEntry;2 deliberately
persists. The first-trial plan now reuses this mechanism. GuardConsumeSetting
previously ignored reset-write and flush failures; it now refuses activation
unless both succeed.40 extracted controls pass; original source fails7.

The gate is consumed per driver load, not each StartDevice. This correction
neither proves repeated same-image PnP safety nor solves production cold-start
clock ordering. The next trial must use1, with a distinct versioned candidate,
strict startup-aware script and fresh clock/temperature preflight.


## 2026-09-23 candidate0794 live trial (M245)

Distinct signed0.7.94.1 installed with hardware gates closed and unchanged Windows boot. Relevant host regressions pass. The startup-aware one-shot PnP trial was launched; SSH became unavailable before result collection. Automatic readiness and an OS GPU workload remain unverified. See facts M245 and candidate0794 evidence. Owner priority: complete the positive startup-to-workload path before expanding failure-case coverage.


### M246: recovery and phase visibility

Owner reset restored display-only operation (FullWddm0). No new crash dump or failing-start ring was retained. Candidate0795 adds KeepLog-gated flushed snapshots before each coordinator phase and after readiness, outside subsystem locks at PASSIVE_LEVEL. Host319 and WDK build pass; candidate not installed. Next positive-path trial must inspect these snapshots before retrying initialization. See facts M246.


### M247: prepare OS completion storage

Candidate0795 reached all coordinator hardware phases but failed final readiness. Source required a lazy GFX fence page and diagnostic-only IB page. Real RUN now allocates its completion page before engine stages; OS jobs do not require the diagnostic IB page. Host319 and WDK build pass. Candidate0796 installed without reboot; its trial lost SSH after PnP enable returned, before readiness observation. Await persisted snapshots; no claim of working OS submission. See facts M247 and startup-storage evidence.


### M249: GFX stage trace

KeepLog now splits startup RUN into existing incremental stage calls and persists logs before/after each, at PASSIVE_LEVEL outside subsystem locks. Untraced startup remains one call. Host144 and WDK build pass. Candidate0797 installed without reboot; trial again lost SSH after PnP enable. Await stage snapshot recovery. The exact hang remains unknown; see facts M249.


### M250: automatic start and first real OS paging verified

Recovered0797 kernel log proves both-engine readiness, WDDM admission and real OS paging completions. First observed submission refusal is STATUS_DEVICE_BUSY for fence13 while seq12 remains in flight (hardware slot11); current path then closes node1 and triggers TDR. The dump also exposes a secondary NULL read in dxgmms2 DMA-history capture. Next positive-path work is proper retention/dispatch of subsequent OS submissions; do not fake completions or remove the ring-space bound. Temporary-builder max_dw messages alone do not establish a fatal alignment defect. See facts M250 and startup0797-dump evidence.


### M251: owned paging FIFO and first application control

Node1 now retains CPU-owned command copies in FIFO order while one SDMA packet executes. Real completion retires its head and dispatches the next; software fences share that order. Queue state gates preemption/rejection; stop joins DPCs before releasing copies. A per-dispatch deadline avoids stale timer expiry against the next packet. Host43 and stop/preemption/watchdog controls pass. Candidate0798 live startup and vkcompute pass:8tests with0mismatches,24/24GFX and3926/3926SDMA,zero timeouts/refusals/TDR. Subsequent stories15M inference lostSSH after SubmitCommandC01E0200; its completion remains unproven. Recover logs before further runs. See facts M251.


### M253: correct ICD selection and successful inference

The elevated0798 harness ignored environment-based ICD selection and used the registry m8 driver. This was a harness regression from ops-gpu-016, which uses a Limited interactive task. Read-only loader comparison confirms both paths. Reusing the identical0798KMD with the hash-matching quiet ICD through a Limited task passes Vulkan and one reference-matching run each of stories15M/TinyLlama. GFX2354/2354 andSDMA24161/24161,zero timeouts/refusals,noTDR. Automatic startup-to-inference is now verified; repeat/pressure/performance and the remaining audit contracts are still open. All future acceptance harnesses must capture the actual loader path and DLL hash. See facts M253.


### M254: eight repeatable inference processes

Four fresh processes per model pass in the same active full WDDM0798 GPU session, alternating stories15M and TinyLlama. All eight native exits are zero, actual loader traces select the hash-verified quiet ICD, all layers are offloaded, and output equals the immutable E14 Linux reference after CRLF normalization only. Cumulative hardware counters: GFX11674/11674 and SDMA120766/120766; zero timeouts/refusals and no TDR. Clock readback1000MHz/VID116; observed temperature70.6-71.6C. No reinitialization or reboot. Repeat acceptance passes; equivalent benchmarks, actual GPU pressure/eviction and the remaining audit/lifecycle contracts stay open. See facts M254 and repeat0798 evidence.


### M255: full-WDDM benchmark baseline

Unchanged0798 and quiet ICD complete b9564 llama-bench pp512/tg128, r3, t6, ngl99 at verified1000MHz/VID116. TinyLlama:1092.70 +/-4.16 prompt tokens/s and113.88 +/-5.00 generated tokens/s. E14 Linux1000MHz reference:1119.59 +/-0.40 and154.92 +/-0.42 (Windows2.40%/26.49% slower). stories15M:37162.42 +/-426.03 and474.49 +/-110.83; generation samples350.30,563.32,509.85 show substantial spread. CumulativeGFX22039/22039 andSDMA150022/150022,zero timeouts/refusals,noTDR;temperature70.6-72.2C. No reboot/reinitialization. Linux and Windows differ in driver/backend stacks; Linux text table does not expose every default and no matched1000MHz stories15M reference is established. Profiling, actual GPU pressure/eviction and remaining audit contracts remain open. See facts M255.


### M256: real GPU readback through residency controls

New gpu-residency-probe uses BC2A/BC2C/BC2S and imported DMA_DATA definitions to copy every source byte through GFX into a sentinel-filled readback buffer, waits for monitored fences, then compares every dword. Built /W4 /WX; help smoke passes. VRAM64KiB: initial read and3Evict/dirty-pressure/MakeResident cycles pass, residency2->1 each cycle and4real GFX completions. Small harness omitted native numeric exit due Process.ExitCode handling; explicit final PASS/fence/readback witnesses are retained. Corrected cmd exit capture used for1GiB.

VRAM1GiB initial readback passes1024fragments; first Evict succeeds but residency stays1for5seconds, so native exit1 occurs before pressure allocation. Final cumulativeGFX23067/23067,SDMA201363/201363,zero timeouts/refusals/noTDR. This is not post-eviction1GiB acceptance or proof of physical relocation. Local MS d3dkmthk.md:12491 defines Evict as residency reference decrement. Probe now creates/dirty-fills competing memory before requiring departure; revised source builds but is not deployed. Next validate that revision, retain strict departure and GPU-content witnesses, then isolate actual transfer operations. See facts M256.


### M257: three1GiB residency cycles with full GPU readback

The revised probe passes64KiB and1GiB controls with native exit0. Each of3large cycles witnesses NOTRESIDENT3 after dirty competing1GiB memory, restores GPU-memory residency1, then verifies every dword through GPU CP DMA readback. Initial plus3post-cycle reads cover4GiB through4096real GFX submissions. Re-residency takes6282/6187/6171ms; earlier5s tool limits were too short for these observed operations. Aggregate map/residency tool waits are60s, individual GPU waits5s; KMD watchdog unchanged. Final cumulativeGFX30247/30247 andSDMA541409/541409,zero timeouts/refusals/noTDR. No reboot/reinitialization.

This closes the tested positive1GiB residency/content-preservation case, not all paging contracts. Physical endpoint tracing, shader cache visibility, GTT/aperture paths, arbitrary aliases, OS lifetime and recovery remain separate. Source/probe revisions2/3 and their failed waits are preserved. See facts M257 and gpu-residency0798-v4 evidence.


### M258: requested-GTT GPU residency controls

Unchanged M257 probe passes GTT64KiB and64MiB, native exits0, three residency cycles each and every word verified through GPU readback before/after cycles.64MiB reaches NOTRESIDENT3 each cycle and returns to GPU-memory residency1. Final cumulativeGFX30507/30507,SDMA550298/550298,zero timeouts/refusals/noTDR. No reboot or GPU reinitialization.

MapApertureSegment/UnmapApertureSegment counters remain0. Do not treat this passing modern GTT allocation path as validation of legacy aperture callbacks. Physical Transfer still rejects segment2 in WddmLocalPagingEndpoint; positive support must resolve aperture backing/alias ownership instead of adding the VRAM base. Physical endpoint trace, cache policy and other audit contracts remain open. See facts M258 and gpu-residency0798-gtt evidence.


### M259: aperture logical-state foundation (host only)

Current physical Transfer rejects segment2; GfxPagingBuildAperture emits delayed GART updates but retains no logical page identities. A later builder must not infer planned mappings from current hardware PTEs. Added portable caller-owned aperture state with copied physical pages, atomic map-batch validation, unmap and one-page resolution.533host checks pass under /W4 /WX, including fragmented mappings/remap/alias/boundaries. Module is NOT wired into KMD or deployed; no runtime support claim.

Next integrate storage lifetime, exact accepted DMA-record publication and serialized lookup, then resolve aperture endpoints through physical system pages so existing alias staging receives true identities. Do not add a VRAM base or presume aperture offsets imply distinct physical memory. Local MS EvictionSegmentSet0 specifies direct locked-system-memory transfers; current0 explains why M258 alone cannot cover an eviction aperture. See facts M259 and aperture-logical-state evidence.


### M260: logical aperture publication integrated locally

VidMm owns512KiB aperture state from StartLayout through Stop, under CpuUpdateLock. MAP/UNMAP publication passes exact emitted Start/Next slices, checks DMA/private capacity before logical commit, copies MDL PFNs without retaining pointers, then advances the accepted record.13689extracted host checks pass including real publication and start/stop ownership; omitting commit caused5failures in the13685-check control. Signed WDK development build passes, not deployed. Physical segment2 endpoint consumption and complete-transfer ordering/aliases still remain. No runtime aperture support claim. See facts M260 and aperture-commit evidence.


### M261: physical aperture endpoints integrated locally

Segment2 endpoints now resolve planned aperture mappings into actual system physical pages, preserving fragmented PFNs and physical alias identity. Existing temporary GART copy/fill machinery handles these pages; a one-page aperture/MDL alias uses VRAM staging. Whole-range preflight rejects missing mappings before a prefix. A paging-builder push lock serializes mapping publication and lookups across each DDI batch.13712host route checks and an extracted wrapper lock-order control pass; WDK DEV build passes, not deployed.

Positive two-page aperture/local transfers and fill are host-verified. Multi-page indirect alias cycles remain refused; OS order across pending/multipass buffers, post-callback page ownership and runtime aperture callbacks are not proven. No overall aperture completion claim. See facts M261 and aperture-endpoint evidence.


### M262: disjoint multi-page indirect transfers

The physical transfer builder now classifies actual physical byte intervals before admitting multi-page copies between distinct MDLs or aperture endpoints. Sorted/merged source intervals and destination binary searches admit fragmented disjoint ranges while retaining rejection of cross-page aliases. Portable tests pass 5000 independent overlap-oracle cases; extracted KMD routing passes 13722 checks, including disjoint MDL/MDL, aperture/aperture and aperture/MDL controls plus a refused page-swap cycle. Signed WDK DEV build passes, not deployed.

An aligned 1 GiB host classification uses about 4 MiB temporary CPU storage, 524288 resolver calls and measured 0.011 seconds; this is not a kernel/GPU performance result. Repeated multipass classification cost, arbitrary alias scheduling, pending-buffer ownership and runtime legacy aperture callbacks remain open. See facts M262 and disjoint-indirect evidence.


### M263: independent indirect multi-buffer byte replay

Added eight host scenarios combining MDL/aperture endpoints with independently limited DMA/private buffers. Actual emitted PTE and SDMA fields drive sparse physical byte backing across repeated DDI calls, including fragmented addresses above4GiB, unaligned aperture boundaries and untouched bytes. All13884checks pass. A generated-code mutation repeating the first MDL source page yields9failures, including four final byte-oracle failures. No production source change, driver deployment or hardware cache/ordering claim. See facts M263 and indirect-multipass-replay evidence.


### M264: positive reused-buffer shader visibility on unit A

Unchanged full-WDDM0798 and verified quiet ICD pass two16-round processes: each round changes1M input words in reused host-coherent allocations, runs two chained integer shaders, and compares all output bytes with the CPU oracle. Intentional stale-input control detects round1 with the previous GPU hash retained. Native exits0/1/0 and independent host validation pass. GFX30541/30541,SDMA673837/673837,zero timeouts/refusals,noTDR;1000MHz,temperature71.0..71.5C. No reboot or driver deployment.

This adds shader visibility evidence beyond CP DMA, but does not identify cache attributes of borrowed page-table pointers or prove shader visibility after eviction, all memory types, or legacy aperture callbacks. See facts M264 and shader-coherency0798 evidence.


### M265: explicit Vulkan memory types pass; cache intent propagation gap

Types2(heap0,flags0x6),3(heap1,flags0x7),5(heap0,flags0xe) each pass16+16positive shader rounds and the intentional stale-input control. All three allocations per process are logged/validated;96positive rounds total. Unchanged0798, GFX30643/30643,SDMA697338/697338,zero faults/noTDR, no reboot. See facts M265 and shader-memory-types0798 evidence.

HOST_CACHED here is a Vulkan property, not a measured CPU mapping attribute. BC2A contains gem_flags, but current UmdBlob allocation view drops that field and the UMD allocation branch unconditionally retains Cached=0. The positive results do not prove Windows write-back mappings for type5. Next preserve RADV cache intent through the contract reader and allocation policy, with command-buffer USWC controls and actual PTE coherency verification. M264's default selector prefers DEVICE_LOCAL; it did not simply choose the first compatible type. Borrowed table-pointer cache attributes remain a separate open question.


### M266: explicit cache intent implemented in both producer and KMD (local)

Windows winsys previously left gem_flags zero, so the M265 gap affects producer and consumer. New BC2A v2 uses the same192-byte layout and explicitly transmits CPU_ACCESS/NO_CPU_ACCESS/GTT_WC intent; KMD preserves the full field and applies cached backing-store policy only to v2+ CPU-accessible GTT without USWC. Version1 retains prior WC behavior. GPU MMU advertises CacheCoherentMemorySupported; existing PTE encoding propagates OS CacheCoherent to SNOOPED. Allocation logs include requested GEM flags and chosen Cached.

4096 heap/flags/alignment cases validate v2 and v1 compatibility; parser/kernel compile-check, PTE regressions, signed WDK DEV build and ICD build pass. Neither binary is deployed. Paired runtime validation, actual OS coherent PTEs and CPU table alias types remain open. See facts M266 and cache-intent-v2 evidence.


### M267: candidate0799 installed, full-start evidence pending

Added system-leaf encoding counters for OS CacheCoherent/noncoherent and AMD SNOOPED disagreement, with host controls.13887routing checks and signed build pass.0.7.99.1 installs under closed gates with matching version/hash,deviceOK,unchanged OS boot. Subsequent PnP disable/enable succeeds at command level but output stops and independent SSH times out before startup status/log capture. No new ICD or GPU application ran. Cause/stage remain unknown; do not infer a cache failure or successful full startup. Owner screen observation requested before reset. See facts M267 and candidate0799-start evidence.


### M268: actual producer/parser compatibility control

The test now extracts the real BC250 allocation constructor/struct from the Mesa checkout and the actual RADV enum definitions.4096heap/flag/alignment cases pass against the KMD parser and independently expected AMD UAPI bits, in addition to existing v1/v2 controls. Omitting producer GEM assignments only in generated test code produces5120failures and exit1. No production change or additional lab operation. M267 startup remains unresolved after another SSH timeout; owner screen observation is pending. See facts M268 and cache-producer-roundtrip evidence.


## 2026-09-23: M267 cold recovery observation

M267 recovery evidence: `evidence/windows/2026-09-23-E27-m9-inference/candidate0799-cold-recovery/`. Owner reports desktop recovery after AC removal following black screens across reboots. SSH confirms boot 15:39:09, FullWddm=0 and CM_PROB_FAILED_POST_START. Last persisted full-start boundary is GFX stage6 (CP), without a completion record. Root cause and exact failing instruction remain unknown. No new GPU test or restart was issued; cache-intent runtime acceptance remains pending.


### M269: retain requested stop capture during upgrades

The local INF now preserves KeepLog with DWORD NOCLOBBER; hardware gates still close on install. GuardLogKeep reads the setting during StopDevice, so an unconditional AddReg zero could suppress old-driver teardown evidence. Standard InfVerif passes with existing warning1199; strict validation reports that same OS-target issue before and after. Not deployed. Retrieved pre-install snapshots stop at the earlier successful startup, not teardown. CP substep logs reside in memory inside the APC-level GartLock; disk snapshots surround whole stages. Missing disk substeps do not identify the failing instruction. See facts M269 and reentry-capture-policy evidence.


### M270: shared CP steps and persistent startup boundaries (local)

The shim CP sequence now has eight ordered steps shared by its ordinary entry point and traced KMD startup. Startup snapshots run outside GfxExecute locks at PASSIVE_LEVEL before and after each step. Internal progression rejects skips and normal RUN past partial CP; readiness remains stage8. Linux replay354+35writes matches;19extracted coordinator scenarios and signed WDK DEV build pass. Test mocks do not prove live locking/durability. Not deployed; runtime diagnosis and full startup/reentry acceptance remain pending. See facts M270 and cp-startup-checkpoints evidence.


### M271-M272: startup and paired cache-intent runtime acceptance

Candidate0.7.100.1 installs as oem74 and starts full WDDM with every CP checkpoint, SDMA and interrupts successful; persisted log confirms the steps. Initial292paging jobs complete. Boot15:39:09 remains unchanged; the session followed owner AC removal, so warm reentry and0799 cause remain open. Legacy/v2 ICDs each pass96positive shader rounds plus3stale controls on types2/3/5, exact loader witnesses and18expected native exits. FinalGFX204/204,paging16284/16284,zero timeouts/refusals/noTDR. V2 interval adds23058coherent system-leaf encoding attempts with zero snoop mismatches; repeats/backgroundOS included. This does not identify actual CPU PAT/borrowed alias types or prove eviction+shader. Both tasks removed, newICD isolated in cache-intent-v2; globalquietICD unchanged. See factsM271/M272 and linked evidence.


### M273-M274: v2 inference and performance

Same07100session/isolatedv2ICD passes Vulkan8operations and exact E14 Linux stories15M96/TinyLlama64token references with full offload. Benchmarkb9564pp512/tg128,r3,t6,ngl99,1000MHz gives TinyLlama1099.25/116.79tokens/s, belowLinux1119.59/154.92. PreviousWindows1092.70/113.88 is a baseline, not causal proof of improvement. FinalGFX12923/12923,paging72525/72525,zero timeouts/refusals/noTDR. No reboot/reinit, tasks removed. CPU PAT/aliases, eviction+shader, arbitrary transfer aliases, OS lifetime and warm reentry remain open. See factsM273/M274 and evidence.


### M275: cyclic page-copy planning foundation (host only)

Added portable permutation planning over normalized distinct physical page identities. Inverse cycle traversal emits save/copy/restore with one scratch page, O(N) planning and at mostN+floor(N/2)actions. All46233permutations through8identities match an independent initial-snapshot oracle; omitting SAVE produces78522payload mismatches. This is not yet KMD admission or hardware support. General unaligned/duplicate/non-permutation aliases remain required. Integration must retain a distinct cycle scratch allocation through final fence and across interleaved/multipass buffers; existing per-slice PagingCopyStaging cannot hold the cycle value. No partial cyclic DMA publication before that ownership is implemented. See factsM275 and page-permutation-plan evidence.


### M276: complete-cycle batching and scratch lifetime boundary

Existing paging FIFO serializes hardware jobs to actual completion. A cycle fully contained in one submitted buffer can therefore finish before unrelated jobs reuse scratch. Local planner now packs whole cycles under an action budget; NeedCycle reports an oversized cycle separately from a productive partial batch, avoiding a design based on endless empty retries. All46233permutations across12budgets pass with scratch overwritten between batches. Tests explicitly enlarge simulated capacity for oversized cycles; no real ring-capacity guarantee. Not integrated/deployed. General split-cycle ownership and physical normalization remain required; this refines M275 lifetime planning rather than closing it. See factsM276 and page-permutation-batches evidence.


### M277: physical identity normalization for page cycles

Local normalizer maps full aligned source/destination physical pages into original-source indices using sorting/binary search, O(N log N). It admits only distinct-page bijections; partial pages, different sets and duplicate identities still require general handling. All46233small permutations/12budgets pass with high physical addresses, including repeated low32bits; truncation mutation fails.262144identity normalization checks every result in0.014hostCPU seconds (not1GiB GPU execution). Not integrated/deployed. Next connect captured endpoint identities and complete-cycle packet budgeting while preserving oversized/general alias scope. See factsM277 and page-permutation-identities evidence.


### M278: complete permutation packet emission (host only)

GfxPagingBuildPermutation now resolves MDL/aperture physical identities and emits an entire page-permutation plan into one submission, with engine-owned VRAM cycle scratch and per-slice staging disabled. Budgeting uses the real emitter reservation (96 DWORDs); emission advances by actual packet size (83 DWORDs). The extracted KMD suite passes 14003 checks including 12 swap/cycle/identity fixtures across all four endpoint combinations and initial-snapshot byte comparison. WDK build passes. No WDDM admission or deployment yet; arbitrary partial/non-bijective/oversized aliases and OS lifetime work remain open. Next integrate the bounded positive path with DDI publication and progress accounting before hardware acceptance. See facts M278 and page-permutation-packets evidence.


### M279: WDDM admission for complete page-permutation cycles

WddmBuildPhysicalTransfer now publishes M278 whole-plan packets for aligned indirect page bijections that fit a single submission. It accounts for DMA/private capacity, advances progress only after publication and emits nothing for a completed token. Host tests call the actual transfer helper for 12 byte-oracle fixtures; 14039 checks pass. Restoring from the original source instead of cycle scratch in the generated negative control produces eight byte-oracle failures. WDK build passes; no deployment or runtime OS alias claim. Non-bijective, partial-page and oversized graphs, general scratch/OS lifetime and restricted DDI errors remain required. See facts M279 and page-permutation-ddi evidence.


### M280: complete-cycle multipass in the actual WDDM builder

The alias path now batches independent cycles across DMA buffers using M276 planning and actual packet/capacity costs. MultipassOffset names the next cycle seed (minimum original source index), not a contiguous completed-byte prefix; terminal value is total page count. Every accepted buffer contains complete SAVE/RESTORE cycles, so another retired job may reuse scratch between buffers. All cycle sizes are checked against fresh hardware capacity before publication; a single oversized cycle remains unimplemented. Host tests pass14343checks with 12 three-buffer MDL/aperture fixtures and scratch overwrite between passes; broken restore control fails20byte oracles. WDK build passes, no deployment/runtime claim. General alias graphs, per-transfer ownership for oversized cycles and OS lifecycle remain required. See facts M280 and page-permutation-multipass evidence.


### M281: oversized whole-page cycles use bounded transpositions

PagingPermutationPlanBounded retains minimal copies for cycles fitting hardware capacity and decomposes larger cycles into three-copy pivot swaps. Each swap finishes scratch lifetime before the next job; no cross-buffer saved value is required. The actual WDDM alias path uses tagged action-index resume tokens and reports logical bytes once the complete plan is built, superseding M280 seed-token accounting. All46233small permutations pass fixed budgets3..9 without growing capacity. Actual-DDI suite14575checks passes, including four8page cycles spanning7buffers; broken restore control fails24byte oracles. WDK build passes, not deployed. The fallback costs3(N-1)copies; rebuilding metadata per callback requires later performance work. Partial/non-bijective/virtual aliases and actual OS endpoint lifetime/cache/recovery acceptance remain required. See facts M281 and page-permutation-bounded evidence.


### M282: general whole-page dependency graphs with distinct destinations

GfxPagingBuildPageGraph replaces permutation-only normalization with a sorted union of physical identities. Reader counts schedule every required original-data read before its source is overwritten; residual cycles use M281 execution. Repeated sources and different source/destination sets are admitted, with duplicate destinations and partial pages still open. All280391normalized graphs at six fixed budgets pass; the actual-DDI suite14643checks passes including four fan-out-plus-cycle physical packet fixtures. Generated direct-copy corruption fails7366164page oracles. WDK build passes, not deployed. Workspace is128bytes per logical page and still rebuilt each callback; performance, virtual alias integration and actual OS lifetime/cache acceptance remain required. See facts M282 and page-copy-graphs evidence.


### M283: live shader-after-eviction control passes on07100

An isolated diagnostic cache-intent-v2 ICD explicitly evicted/restored a4MiB input before rounds1/5/9; observed residency1->2->1 with paging fence completion each time. The two-dispatch integer-hash test reads input before CPU remapping and matches all1048576words in16rounds. Baseline also passes; stale-input negative control fails after its own eviction/restoration. Exact Limited-task loader/submit witnesses retained. FinalGFX12957/12957,paging216936/216936,zero timeouts/refusals/noTDR; no OS restart. This is shader cache/data coverage after measured shared-memory residency, not physical-PFN relocation or pressure-scale proof. Installed07100 does not include localM278-M282 alias changes. See facts M283 and shader-eviction07100 evidence.


### M284: identical destination assignments are coalesced

The page graph admits repeated source/destination pairs by counting each distinct copy once. This preserves dependency scheduling and avoids duplicate traffic. Conflicting sources for one destination remain unsupported. Exhaustive classification of69904small edge lists yields19072consistent lists, all accepted with correct snapshot output. The actual-DDI suite14703checks passes, including repeated-edge swaps across four endpoint combinations. WDK build passes, no deployment or runtime claim. Partial ranges, virtual aliases and OS lifecycle/cache/recovery/performance remain open. See facts M284 and page-copy-duplicates evidence.


### M285: partial-page byte-band planning foundation

The inspected Microsoft TransferSize fields specify bytes without an explicit page-multiple guarantee. For equal source/destination in-page offsets, PagingPageBands partitions the request into at most three disjoint byte-offset bands with logical page ranges. Each band can use the existing page graph with a uniform partial copy length, avoiding byte-per-node metadata.32768coverage cases and8192full-byte band/graph composition cases pass, including untouched bytes and scratch reuse. Not connected to actual packet emission or WDDM multipass yet. Unequal offsets and runtime acceptance remain required. See facts M285 and partial-page-bands evidence.


### M286: partial-page bands integrated into WDDM packets

The page graph now captures bounded first/last-page identities, preflights all equal-offset bands and emits their byte offsets/lengths. Tagged progress encodes band plus action; available capacity is used across band boundaries.15648host checks pass, including25partial-byte fixtures and384DWORD resumes with identical copy semantics and no scratch lifetime crossing a buffer boundary. Broken restore control fails88checks. WDK build passes; no deployment. This closes local packet support only for equal-offset partial graphs. Unequal offsets, conflicting destinations, virtual aliases and actual OS lifecycle/cache/recovery/performance acceptance remain open. See facts M286 and partial-page-packets evidence.


### M287: bounded physical cache query for borrowed tables

WDK26100 exposes MmGetCacheAttribute with a physical-address input and explicit status/type output. Initial CPU_VIRTUAL table updates now log that result under their existing lifetime lock after physical-extent validation. The result does not change mapping or update admission and is not interpreted as a per-VA PAT measurement.15684host checks and real WDK linking pass; no deployment or live query result. Removing the retained map would also require preserving bootstrap, CPU Present and IB diagnostic walks. M190 remains open pending actual alias-attribute evidence or a complete replacement of those mappings. See facts M287 and table-cache-query evidence.


### M288: candidate07101 installed; full startup unresolved

Candidate0.7.101.1 includes localM278-M287. Exact package passes25checks, installs as oem75 with closed hardware gates, verified hash/version, deviceOK and unchanged Windows boot. One subsequent traced full-WDDM start lost SSH observation; its original host process remains live and independent reads time out. No last-stage evidence is available yet. Do not infer a CP/cache/alias cause or retry the startup. Owner monitor observation and read-only persisted-log recovery are pending. Warm reentry/full M9 remain open. See facts M288 and candidate07101-start evidence.


Recovery update for M288: the owner reports a desktop after disconnecting AC power. Remote recovery still times out; independent probes of both configured network routes do not reach TCP22. Local SSH service inspection is requested. No new hardware startup or reset was issued. Persisted checkpoints and installed runtime state remain unverified; cold recovery is not warm-reentry acceptance. See candidate07101-cold-recovery evidence linked in facts M288.


### M289: shared graph identity normalization (host only)

The equal-offset partial-copy builder normalizes the complete captured physical-page union once per callback and selects each band's edge subspan for validation/emission. This removes up to five repeated sorts without retaining state across OS calls. Actual KMD packet tests pass15684checks and the WDK build passes. A262144page host planning benchmark has matching semantic move digests and median187ms before versus48ms after (three trials); this is not a GPU/inference gain. Allocation remains128bytes per page and metadata is rebuilt per callback. No deployment while SSH remains unavailable. Full M9 runtime, cache, lifetime, general aliases and warm-reentry acceptance remain open. See facts M289 and shared-graph-normalization evidence.


### M290: virtual cross-page aliases fail the byte oracle

An explicit host regression now reproduces the unresolved virtual alias gap: source[A,B,C] to destination[B,C,A] through distinct VAs returns builder success but corrupts initial source bytes when actual SDMA packets execute in their emitted order. Disjoint copies pass the same translator and decoder. The opt-in `paging_packets.exe --virtual-alias-ordering` has32checks/1failure (native exit1); default15684regressions pass. This is a synthetic translation/actual packet construction test, not hardware evidence. Production correction is pending: capture and schedule dependencies for the whole transfer before publishing a prefix, while preserving table-shadow commits for local/table destinations. Reusing physical graph publication without those commits is insufficient. See facts M290 and virtual-alias-ordering evidence.


### M291: virtual system-page graph integration (local)

Equal-offset system/system virtual transfers now capture PFNs through the paging root and share the physical graph/band scheduler before any prefix publication. Tagged resumes and per-buffer scratch-complete groups handle bounded DMA capacity. M290's byte corruption is corrected for this path:15854default checks and170focused checks pass, including partial bytes and overwritten scratch between submissions. The broken-restore control fails10focused checks. WDK build passes; no deployment. Local/mixed endpoints retain the table-shadow-aware path, whose general cross-page aliases still require a correction; unequal offsets and actual OS/GPU lifetime/coherency/reentry acceptance remain open. Capturing and sorting all virtual system transfers also adds metadata cost that must be profiled and optimized. See facts M291 and virtual-system-graphs evidence.


### M292: graph acceptance through actual table walks (host)

The virtual graph byte oracle now includes real extracted VidMm hierarchy initialization and logical walks, as well as accepted GPU-PTE remaps whose retained GPU-visible table image is still old. Eighteen fixtures cover disjoint/cyclic/partial copies and two ring capacities across three translation modes. Default16348checks and focused664checks pass; omitting logical update publication causes17focused failures including byte corruption. This strengthens M291 construction-order evidence without claiming hardware execution or OS lifetime acceptance. Production source and installed driver are unchanged. General local/mixed aliases, unequal offsets, metadata costs, cache and recovery still require work. See facts M292 and virtual-graph-real-walk evidence.


### M293: logical table scratch foundation

PagingPtShadowSaveBytes/RestoreBytes preserve both values and Known-byte metadata through complete copy cycles using a separate caller-owned scratch slot. Existing byte-copy semantics share the same inner loop. Host2434928checks (including124new full/partial cycle fixtures), kernel compilation and16348actual-routing checks pass; a generated broken restore fails118403checks. This helper is not wired into graph publication and is not hardware acceptance. Integration must reserve scratch outside the kernel stack, serialize accepted groups, and preserve captured physical plans across any multipass copy that changes its own translation tables. Re-resolving modified VAs is insufficient. MS local d3dkmddi.md4132/4144 guarantees progress-field preservation, not arbitrary retained-pointer lifetime. Local/mixed/table aliases and full M9 remain open. See facts M293 and table-shadow-scratch evidence.


### M294: planner-to-shadow batch composition (local)

PagingPtShadowApplyPageMoves now consumes actual planner identity indexes and complete batches, preflights them before mutation, and applies SAVE/COPY/RESTORE in emitted order. System sources are unknown metadata; ordinary unregistered destinations remain unregistered. Eight-page cycles pass independent snapshot checks with full/partial bands, local/mixed metadata and one/seven buffers despite scratch overwrite between batches. Host2672416checks and routing16348checks pass; a reversed-order mutation fails61361checks. WDK build passes, not deployed. This API is not yet called by WDDM publication: matching accepted GPU packets, preallocated scratch and stable captured plans across table-changing resumes remain required. See facts M294 and graph-shadow-batches evidence.


### M295: graph commit in actual paging publication

The actual private/DMA publisher now accepts an optional callback-scoped graph batch, validates buffer capacity before applying it, then publishes exact command bytes. VidMm owns embedded scratch and serializes the complete logical batch under its existing exclusive lock. A three-table cycle built with actual copy packets passes independent packet replay, all1536logical entries, refused-capacity atomicity and unchanged live-backing checks. Routing22513/focused6165checks pass; omitted graph commit fails1536checks. WDK build passes, no deployment. Existing production DDI builders still pass NULL for the graph: general local/table endpoint capture, matching emitted batches and retained plans across table-changing resumes remain required. See facts M295 and graph-shadow-publication evidence.


### M296: capture ownership and context teardown

Contexts now contain a CPU-only capture owner with monotonic tagged identifiers, exact detach and drain operations. Tokens are not reused after completion or drain. Actual context destruction joins PagingBuildLock before object cleanup; object/adapter teardown drains remaining captures outside the spin lock. Owner46checks and actual extracted release64checks pass; WDK build passes, not deployed. Builders do not attach captures yet. Immutable endpoint capture, matching graph batches, completion detach and OS cancellation/concurrency remain required; this infrastructure is not general alias acceptance. Local MS d3dkmddi.md30891 and6290-6325 support the context handle and cleanup obligation, not a claim that arbitrary captured endpoint lifetimes are proven. See facts M296 and paging-capture-owner evidence.


### M297: immutable virtual graph capture

The new capture builder resolves equal-offset source/destination ranges once, retains normalized physical identities and local/system flags, and preflights every byte band. A resumed planner uses those immutable indexes and proposes the next cursor without advancing it before publication. Six system/local/mixed full/partial cycle fixtures pass after translation is disabled and mappings replaced, with scratch overwritten between batches. Routing22655checks and WDK build pass. Storage currently costs header+136bytes/page; it is not yet DDI-wired or deployed. Next work is captured-batch packet emission and exact graph publication, then owner attach/lookup/key validation/advance/detach. Unequal offsets, conflicting access-domain aliases and actual OS cancellation/lifetime remain open. See facts M297 and retained-graph-capture evidence.


### M298: retained graph packet emission

The retained emitter now converts captured local/system identities into actual direct/GART copy transactions, returns the matching graph descriptor and proposed cursor only on success, and leaves owner progress unchanged. Six full/partial local/mixed/system fixtures replay actual7/83DWORD packets after VA translation is disabled, with correct bytes/access domains and no scratch value crossing a submitted batch. Routing22817/focused304checks pass; broken restore control fails16checks. WDK build passes, no deployment. The DDI still needs owner attach/lookup/key validation, per-batch publication and accepted-cursor advance across bands, plus completion/cancellation ownership. See facts M298 and retained-graph-packets evidence.


### M299 - Context-owned virtual graph publication (host only)

The virtual transfer dispatcher now uses the retained graph owner for equal-offset transfers larger than a page. Accepted publication precedes cursor advancement; final construction releases CPU-only metadata. Six owned system/local/mixed full/partial fixtures replay actual packets after VA translation changes, with exact private coverage and refusal preserving progress. Routing23109 checks and WDK build pass. See facts M299 and owned-graph-route evidence. This development build is not installed. Unequal-offset dependencies, OS lifetime/cancellation, restricted status policy, hardware initialization and performance acceptance remain open. SSH remains unavailable; recover the installed 0.7.101.1 persisted checkpoints before another hardware trial.


### M300 - Self-table transfer across callbacks (host)

The context-owned route now has an actual-walker fixture whose transfer overwrites its own leaf table. The first full-copy callback changes source translation and requires resume; subsequent packets still use the captured physical identities. Full and partial copies match the original snapshot, and every callback matches all1536 logical entries to independent packet replay. Routing30909checks pass; omitted logical publication fails2080focused checks. See facts M300 and owned-self-table evidence. Tests only: no new driver build or deployment. Live OS/GPU lifetime, cache, initialization, general unequal-offset aliases and performance remain open.


### M301 - Reuse the current byte-band plan

Resumed captured transfers no longer rebuild the full dependency graph for each DMA batch. PlannedBand and MoveCount retain the current Moves array until a band change. Fourteen fixture counters bound emission planning to once per new band; routing30923checks pass. Restoring repeated planning fails exactly14count assertions while byte checks remain passing. WDK build passes, not deployed. This proves fewer planner calls, not a measured throughput increase. Per-page storage remains136bytes; general unequal-offset aliases and live OS/GPU correctness/performance acceptance remain open. See facts M301 and cached-band-plan evidence.


### M302 - Compact retained capture storage

Capture-only source/destination and sorting arrays now share storage with later planner workspace and cached moves. Persistent identities/indexes/domain flags remain disjoint. Allocation drops from header+136bytes/page to header+84bytes/page;14actual allocator assertions and routing30937checks pass, including self-table resumes. WDK build passes; no deployment or measured GPU performance gain. See facts M302 and compact-capture evidence. General unequal-offset aliases, OS lifetime/status/cache/initialization and performance acceptance remain open.


### M303 - Unequal-offset alias regression

Opt-in --unequal-virtual-alias reproduces cross-slice byte corruption for8192bytes shifted by one byte in physically aliased pages. The actual fallback builder reports success; independent83/100DWORD packet replay fails the original snapshot. Disjoint control passes (combined44checks/1failure), default30937checks remain passing. Microsoft TRANSFERVIRTUAL fields describe byte sizes and paging-process VAs; the inspected local/online descriptions do not guarantee equal endpoint offsets or specify overlapping-source snapshot semantics. Treat this as a concrete driver robustness gap, not a proven Windows-issued sequence. Next work needs whole-transfer physical interval dependencies, captured addresses and scratch-complete batches. See facts M303 and unequal-virtual-alias evidence. No production change or deployment.


### M304 - Prove monotone ordering from physical identities

PagingPageAliasDirection checks injective source/destination page lists and matching logical slots for shared physical identities. This permits forward/backward ordering for fragmented pages with unequal byte offsets in linear time; it does not use VA ordering as proof.32snapshot fixtures and routing31002checks pass; WDK build passes. The helper is not DDI-wired. M303 still requires retained slice capture, correct reverse multipass and exact publication; other dependency shapes require the general interval planner. See facts M304 and monotone-alias-direction evidence. No deployment.


### M305 - Retained unequal-offset capture and slice selection

Endpoint capture now handles different page counts and retains normalized physical identities before any publication. Unequal offsets use the physical monotone proof; a new slice selector proposes page-bounded forward/reverse progress without retranslation or owner mutation. Two capture fixtures preserve original bytes after translation is disabled; routing31026checks and WDK build pass. This is not yet unequal-offset packet/DDI integration: M303 remains open. Next connect exact physical slice emission, logical publication and accepted cursor advancement. General interval cycles and live OS/GPU acceptance remain required. See facts M305 and linear-capture evidence.


### M306 - Captured unequal-offset DDI integration

The owned virtual-transfer builder now emits retained physical slices in proven forward/backward order, commits applicable table metadata with exact publication, and advances only accepted progress. M303 shifted system-page alias and disjoint control pass actual packet replay across multiple DMA buffers after translation is disabled. Focused54/routing31080checks and WDK build pass, not deployed. The legacy unequal-offset resolver is no longer selected by the production dispatcher. General interval cycles still lack a planner, and internal unsupported/error status policy, local/mixed unequal packet acceptance and live OS/GPU lifetime remain open. See facts M306 and linear-ddi evidence.


### M307 - Unequal-offset direction and memory-domain coverage

Twelve actual owned-transfer fixtures now cover alias/disjoint ranges, both one-byte shift directions and system/mixed/local physical pages. Independent7/34/83/100DWORD replay checks packet domains, scratch-complete transactions, original bytes, private coverage and capture release across callbacks after translation is disabled. Routing31382/focused356checks pass. Forcing forward order fails exactly3alias byte snapshots. Tests only, no new binary/deployment. Linear self-table publication, general interval dependencies and actual OS/GPU acceptance remain open. See facts M307 and linear-domain-coverage evidence.


### M308 - Linear table publication per callback

Four additional alias/disjoint/direction fixtures initialize actual local table storage using the production layout. Every callback compares3072logical entries with independent packet replay and verifies unchanged live backing. Publication refusal preserves capture and DMA progress; the same capture resumes after restoring the gate. Routing129862/focused98836checks pass; omitted logical commits fail6158focused checks. Address capture uses the synthetic mapper, so actual self-mapping linear traversal remains unproven. Tests only, no deployment. See facts M308 and linear-table-publication evidence. General interval planning and live OS/GPU acceptance remain open.


### M309 - Shared-page position dependencies

The linear direction proof now includes physical page positions as well as byte offsets. Shared pages may occur at different logical indexes if all dependencies permit one traversal direction; conflicting signs remain for the general interval planner.1778accepted snapshot fixtures and additional shifted-list actual DMA/table fixtures pass, routing233887checks and WDK build PASS. No deployment. This expands supported successful transfers without claiming arbitrary alias cycles or live OS/GPU acceptance. See facts M309 and physical-direction evidence.


### M310 - Repeated-source readers in direction proof

The monotone classifier now considers first and last read positions for each physical source identity, allowing repeated source pages when all dependencies permit one traversal direction. It reuses existing contiguous Readers/Writer workspace; capture allocation size is unchanged.8600accepted snapshots and added source[A,A,B]/destination[C,D,A] actual DMA/table fixtures pass, routing304073checks and WDK build PASS. General conflicting-direction dependencies and repeated destinations remain open; no deployment or live OS/GPU acceptance. See facts M310 and source-readers evidence.


### M311 - Linear self-table mapping changes across callbacks

Two fixtures now capture through the actual initialized VidMm hierarchy. The first accepted forward-copy buffer overwrites its own leaf mapping and requires another callback; retained physical identities still produce the original byte snapshot. Both directions pass per-callback1536logical PTE comparisons, unchanged live backing, exact packet/private coverage and ownership checks. Routing328759checks pass. Tests only, no deployment or GPU execution claim. General interval cycles/repeated destinations and live OS status/lifetime/cache/initialization/performance acceptance remain open. See facts M311 and linear-self-walk evidence.


### M313 - Actual07101startup evidence recovered

Owner-supplied changed target address restores pinned SSH access. Read-only acquisition and original file pulls recover successful GART/PSP/IH/GFX1..5 followed by CPstep1entry without persisted completion. Current FullWddm0/stage90/counter2/PnPfailed-post-start; no new hardware action. Prior old-endpoint failures do not prove sshd was down. Next inspect the CPstep1wrapper/KIQ sequence and guard state; exact hang cause remains unproven. See facts M313 and candidate07101-recovered evidence.


### M314 - Guard and CP1 boundary review

Current Stage90/counter2 identifies guard refusal by the source mapping. The log escape fails, so it provides no current ring. CP1entry is persisted before GfxExecute acquires its locks; the KIQ operation has not been isolated. The prepared single display-only PnP recovery closes all hardware gates and performs no full GPU start. Automatic approval rejected execution; explicit approval is pending, and no registry/PnP mutation occurred. See facts M314 and guard-display-recovery evidence.


### M315 - Display-only recovery succeeds

After explicit owner approval, the prepared single closed-gate PnP restart succeeds. Device OK/CM_PROB_NONE, Stage50, info/log exits0 and unchanged Windows boot establish restored diagnostics. All hardware gates remain closed; no new deployment or full GPU start. This supersedes M314 pending approval, not the unresolved CP1 startup finding. See facts M315 and guard-display-recovery/approved-run.log.


### M316 - CP1 checkpoints prepared and replayed

Optional KIQ callbacks identify MQD/selection/register boundaries. Traced unpublished CP1 retains the same mutex and lock order using the documented critical-region ExAcquireFastMutexUnsafe pair, preserving PASSIVE_LEVEL for synchronous checkpoint files; normal paths are unchanged. Default and traced Linux replay match354+35writes, with10ordered cold-path checkpoints and all four negative controls discriminating. WDK build passes. No hardware deployment; persistence, lock/I/O behavior and hang location remain unverified. The development image also includes prior undeployed paging changes. See [M316 evidence and Microsoft contracts](../../evidence/windows/2026-09-23-E27-m9-inference/kiq-checkpoints/README.md).


### M317-M318 - Current hardware startup and shader residency acceptance

Candidate0.7.102.1 installs and completes CP1..8 without an OS reboot. Durable CP1 checkpoints/KeepStatus0 establish that the diagnostic callback executes in this session; the active-queue recovery branch was not taken. Prior07101 cause and warm reentry remain unproved. Physical table cache queries return0xC0000141, not a valid cache-policy witness.

Same-session shader baseline16rounds and eviction16rounds match the CPU oracle; three positive1->2->1 residency cycles, with stale-input negative control failing as intended. Final graphics34/34 and paging15223/15223, no timeouts/refusals/TDR;1492virtual-transfer calls, individual dependency shapes not recorded. Initial script decoding errors were preserved and acceptance cross-checked from native files, installed identity and persisted logs. See facts M317-M318. General alias/cache/lifetime/resource-status/performance acceptance is still open.


### M319 - Residency scale result on07102

64KiB control passes3cycles and4GPU byte-oracle readbacks.1GiB trial reaches the probe300s watchdog after2complete matching readbacks; full acceptance fails. Finalgraphics2495/2495,paging191078/191078,zero driver timeouts/refusals/TDR; sameboot and deviceOK. Changed stdout transport is an unproved timing hypothesis; native-file harness matching M257 is prepared with unchanged deadlines, not yet run. See facts M319. Preserve the working GPU session.


### M320-M321 - Large residency acceptance and current inference baseline

Restoring native-file output permits unchanged1GiB probe/300sdeadline to complete: all3NOTRESIDENT->GPU cycles and4fullword-oracle readbacks pass,4096GFXjobs. This establishes the observed large residency/content path on07102; arbitrary dependencies, PFN relocation and alias cache remain separate. Same-session TinyLlama1082.32/115.06 tokens/s remains below Linux1119.59/154.92 with existing configuration caveats. Finalgraphics16960/16960,paging425844/425844,zero timeouts/refusals/noTDR. NoGPUreinit/Windowsreboot or globalICD change. See facts M320-M321.


### M322 - Capture storage separated from allocation

Exact sizing and caller-owned in-place capture are implemented. Existing DDI behavior remains through a transitional allocating wrapper, so the resource-status gap is not closed. Actual-source328789checks include poisoned reuse with allocation unavailable, then existing packet/shadow oracles; WDK build passes. Microsoft system-paging1GiB geometry provides a reservation bound, but per-context concurrent-capture ownership must be established before integration. See facts M322 and capture-inplace evidence. No lab deployment or reset.


### M323 - System-context reservation integrated locally

SystemContext admission now reserves nonpaged21MiB plus capture header. The actual
captured-transfer DDI uses idle storage, retains it across callbacks and reuses it
on completion; context/stop drain releases active heap captures and reservation
once. Actual-source329129checks and WDK26100build pass. Interleaved busy-reservation
or oversized requests retain the allocating fallback, so the full resource/status
gate remains open. Host fixtures do not establish Windows context/stop concurrency.
See facts M323 and capture-reservation/RESULT.md. Development only, lab07102 unchanged.



### M324 - Prepared context publication

WddmNewContext reserves before shared-list admission. WddmNewObjectPrepared
transfers capture ownership under the list lock only when admission succeeds;
refusal leaves it for caller cleanup. Actual-source list observer verifies the
reservation is present at first publication, ordinary contexts have none, and
allocation/stop refusals retain no allocation.329141checks and WDKbuild pass.
This closes the local partial-reservation publication window introduced in M323,
not all Windows callback/adapter-lifetime concerns. See facts M324 and
capture-admission/RESULT.md. Busy/oversized fallback and remaining acceptance
gates are unchanged. Not deployed; no lab action.



### M325 - Candidate07103 runtime transition unresolved

Candidate0.7.103.1 installs with closed gates, exact SYS/version and deviceOK,
no Windows reboot. Old07102 stop reaches stage79 with halt-register/PSP/GART
witnesses; see candidate07103/previous-driver-stop.log. One subsequent full
start returns PnPenable success then stops producing status; all3configured
SSH endpoints failTCP. Host session21191 remains live at observation. Owner
monitor question pending, no reset requested. Do not rerun or infer CP1 failure.
Recover persisted startup before the next hardware action. Reservation runtime
acceptance is missing. See facts M325/candidate07103/RESULT-1.md.



### M325 recovery blocked after three observations

The initial failed start observation and two consecutive follow-ups find all three
configured SSH endpoints unavailable. Original session21191 remains live without
new output; it has not been classified terminal and must not be relaunched.
Owner monitor/mouse response is still pending. Read-only recovery script is ready
and parser-checked. The next hardware diagnosis requires persisted startup logs;
changing startup based on an assumed failure stage would be unsupported. Full M9
is unachieved. Goal blocked pending owner observation or restored connectivity.
Evidence: candidate07103/recovery-observation-3.json. No reset performed.


### M327 - Finer scheduler access checkpoints, local

M326 narrows persisted progress to the scheduler call. Local AMD code confirms
one original RLC_CP_SCHEDULERS read/write pair. New scheduler-read/write/done
callbacks bracket those accesses without extra MMIO;13callback ordering and
exact354+35write replay pass, as do default replay and WDKbuild. This is diagnostic
preparation, not a fix or hardware acceptance. Persistence itself remains a
possible boundary; do not infer a precise hanging instruction from a missing
snapshot. See facts M327 and scheduler-checkpoints evidence. Not deployed.


### M328 - Current07104 startup succeeds in recovered boot

Verified candidate0.7.104.1/SYS9EE1AA001125B414DC7CBB0382ABF6838427223D8130F1A89505D402D32B69CF
installs and completes one full startup without Windows reboot (boot01:46:31).
Scheduler-read/write/done checkpoints and both-engines-ready appear. System context
reserves22020392bytes; initial paging298/298 with0timeouts/refusals/noTDR.
No captured-transfer use witness or07104content test yet. Prior07103warm failure
remains unresolved; diagnostic timing is not a proven fix. See facts M328.



### M329 - Shader/eviction content acceptance on07104

Same-session M318 workload passes baseline16rounds and eviction16rounds;3positive
residency cycles, stale control detects round1 after its cycle. Independent native
file/loader/submit/byte-oracle validation passes. Graphics34/34,paging15920/15920,
zero timeouts/refusals/noTDR. No restart. Capture-use witness remains missing:
first-four diagnostic lines are not present in snapshots after substantial ring
wrap. No inference of zero/use from absence; expose aggregate path counters before
claiming runtime reservation acceptance. See facts M329.



### M330 - Warm07104 transition pending recovery

After M329 workload, one same-binary full PnPstop/start reports enable success
then loses status/SSH. Full local subnet SSH search finds no pinned lab identity.
Originalsession55183 still live; owner monitor observation pending, no reset yet.
Recover persisted stop/start logs before inferring a failure stage or changing
hardware sequencing. M328 first startup remains a narrower success, not warm
acceptance. See facts M330 and warm07104/RESULT-1.md.



### M331 - Retirement evidence boundary

Source review finds no post-stop RLC progress/idle witness: shim clears enable,
while EnginesHalted observes CP/MEC/SDMA only and PspStop uses that enclosing
quiet result. Existing E13 reload evidence is incomplete, not a successful
reference to copy. See facts M331/retirement-reference-review. Recover M330 logs
before choosing a change; no speculative delay, register predicate or reset.



### M335 - Capture use remains observable after log wrap

Adapter-lifetime atomic totals now distinguish reserved and heap capture plans in
WddmSummaryOf. Actual-source329165checks and WDK build pass; totals survive owner
teardown and count new plans rather than multipass callbacks. Undeployed: M329
runtime reservation acceptance remains open until the new summary is captured.
Warm reentry is still unresolved; E28 also fails on Linux after witnessed RLC
stop. Local GFX10 resume does not invoke its defined RLC reset callback, so that
function alone is not a validated recovery sequence. See facts M335.


### M336 - Reserved capture use witnessed on07105

Installed exact105/fulltable in boot03:35:18 without reboot. First full startup
passes. Same M329 workload and independent native-file validator pass:16baseline
and16evictionrounds,3cycles, expected stale-control failure. Summary reserved plans
increase0->20 with heap0; graphics34/34 and paging4768/4768, no timeout/refusal/TDR.
This closes the M329 missing-use witness for this workload. Busy/oversize fallback,
OS concurrency, broader pressure, performance and warm reentry remain open.
Lab remains in full WDDM105; do not treat this as a successful repeated GPU start.
See facts M336 and candidate07105 evidence.


### M337 - Same-session1GiB acceptance

Full105 passes64KiBcontrol and1GiB three-cycle eviction/readback validation.
Reserved plans26->50,heap0;4096GPUreadback jobs;finalgfx4134/4134,paging180716/180716,
noerrors/TDR. No restart. General PFN/concurrency/cache/reentry acceptance remains
open. See facts M337 and gpu-residency07105-native evidence.


### M340 - Retirement observer prepared

Same104 first/warm PSP reports match all11commands except timing, allrc/status0.
No reported PSP rejection explains the warm hang. E28 lacks a complete post-stop
GRBM_STATUS2 witness. New read-only RLC retirement snapshot builds locally; no
reset or quiet-predicate change, not deployed. Next capture the new observation
before choosing a recovery sequence. See M340.


### M341 - Warm RLC state observed

106warm startup fails after successful PSP; before/afterPSP RLC_CNTL0 and
GRBM_STATUS2 0x01000008 (RLC_BUSY set), readsstatus0. Lastscheduler-read.
OneACrecovery and closed-gate device recovery restore diagnostic106/boot04:16:46.
Next obtain successful-start control before interpreting this as a discriminator
or adding a reset sequence. See facts M341.


### M342 - Busy transition is later than observed GFX stop

Same106firststart succeeds with busy clear;64KiBprobe passes. AfterGFXstop busy
remains clear; post-teardown read sees it set. Next snapshot afterPSPunload/before
GARTrestoration. No newreset or warmreentry acceptance. See facts M342.


### M343 - Busy transition before PSP unload

107firststart/64KiBcontrol pass. AfterhaltSTATUS2 0x8;afterGFXcleanup0x01000008,
unchangedbefore/afterPSPandafterGART. Narrow TearDown/unbind/TLB/resource release
and time; do not infer GpuMemRelease alone from snapshot label. Current107
display-only,boot04:30:34. See facts M343.


### M344 - First retirement GFXHUB flush interval

108first-start/64KiBcontrol pass. During stop, CNTL0/STATUS2 0x8 remains after
first GTT unbind; after GFXHUB flush STATUS2 becomes0x01000008 and remains set.
Physical memory release, PSP unload and GART restoration follow. All reads0.
Successful after-PSP startup also has busy with CNTL1: busy alone is not a hang
predicate. Inspect AMD RLC_NO_KIQ wrappers/GFXOFF/KIQ ordering next; do not skip
invalidation to clear a symptom. Current108display-only,boot04:46:20. See M344.


### M345 - Retirement phase split remains required

Source comparison eliminates a missing bare-metal RLC_NO_KIQ wake handshake.
Linux retires hardware, including GFXHUB context/cache disable, before software
release. Its PCI-unplug guard is not a Windows unbind exemption. See facts M345.

The next implementation must retain GFX allocation ownership while hardware
retirement proceeds, distinguish PnP stop from diagnostic Fini with live IH/GART,
and release storage only after applicable translation/cache retirement succeeds.
Keep the existing no-release-on-unconfirmed-stop behavior. Do not detach Gfx or
GpuMem early if the next phase still needs them. Host transition checks must cover
that ownership across phases; hardware acceptance requires the first-start byte
control and repeated startup after workload. Linux reload still fails on unit A,
so matching phase order alone is not completion. Any isolated RLC reset trial
needs its own source-derived preconditions and control; the ordinary resume path
does not invoke the reset callback.


### M346 - Internal halt/storage extraction, not yet PnP integration

HaltEngines now preserves GFX ownership/storage; ReleaseStoppedStorage owns
the existing destruction path. Fini composes both in the original order.
1700actual-source checks and WDKbuild pass; no deployment. Both PnP stop and
startup failure unwind still need the full phase separation. GartStop cannot
simply move earlier: it destroys the adev/table owner that GFX teardown needs.
Add a distinct GART hardware-retirement operation while retaining that owner.
See facts M346; warm reentry remains unresolved.


### M347 - PnP/startup unwind phase integration prepared

109candidate now performs IHstop/GFXhalt/PSPstop/GARThardware-disable before
GFXstorage cleanup and final GARTrestore. Owners survive until their required
phases complete; unchanged diagnostic Fini and TLB invalidations remain.
3620stop/319startup/25package checks and WDKbuild pass. Not deployed. Hardware
first-start/byte-control/stop witnesses and subsequent warm reentry remain open.
See facts M347 and retirement-phase-integration/RESULT.md for exact procedure.


### M348 -109hardware ordering accepted narrowly; warm reentry still fails

109first full start/64KiB control and PSP/GART-before-GFX-storage stop pass.
Busy stays clear through GARTdisable but sets after first GFXHUBflush. One warm
start losesSSH:11PSPcommands success,CNTL0/busyset,lastCP1scheduler-read. OneAC
recovery, then closed-gate PnPrestart restores109display-only,stage61,boot05:20:01.
No owner action pending. Next isolate RLC recovery with original AMD sequencing;
current evidence is not an exact fault-instruction or reset-fix proof. See M348.


### M349 - Isolated RLC reset candidate prepared, not deployed

Candidate110 adds a default-off EnableRlcReloadReset experiment before PSP load.
Original AMD callback body comparison, ordinary354+35write replay,11reset-model
scenarios,327startup checks and25package checks pass. Model busy-clear is an
assumption, not hardware evidence. The gate requires disabled/busy RLC and all
CP/MEC/SDMA halt bits; post-reset busy or sequence fault refuses startup. No
resource-release or DMA-idle contract is weakened. See facts M349 and
rlc-reload-reset-preparation/RESULT.md for limits and hardware acceptance.
M348 remains the latest lab result; repeated startup is unresolved.


### M350 - Isolated RLC reset trial refuses; lab recovered without reboot

110first start with reset gate0 and unchanged64KiB GPU control pass. Warm gate1
trial observes all CP/MEC/SDMA halt preconditions, but the imported callback's
immediate postcondition still reads RLCbusy; helper-62 refuses before PSP load.
SSH survives; closed-gate PnPrestart restores stage61 display-only in the same
05:20:01boot, then CLIconfirm clears the guard counter. No AC or OS restart.
See facts M350/candidate07110/RESULT.md. This is not proof of reset-register
blocking or DMA activity. Review AMD reset-domain sequencing next; do not
repeat unchanged110warm or weaken the refusal. Warm acceptance remains open.


### M351 - Reset dispatch reference limitation

Local Linux6.18.0 NV need_full_reset always returns true, bypassing generic IP
soft reset in ordinary recovery. Cyan Skillfish PPT lacks SMU MODE1 support;
PSP11.0.8 lacks a MODE1 callback, and the shared wrapper returns0 if absent.
This is source evidence, not exact E28runtime tracing or a working reset.
See facts M351/reset-dispatch-review. Keep110reset gate closed. Review explicit
reset readback ordering before adding reset domains; RLCbusy does not justify
CP/GFX reset bits. Broader startup/DMA acceptance remains open.


### M352 - Candidate111 explicit reset readbacks prepared

EnableRlcReloadReset2 selects AMD-derived post-write reads before both50us
waits;0off and1originalcallback remain. Existing halt guard/busy postcondition
remain, no broader reset domains.22model scenarios distinguish access ordering;
ordinary354+35replay,327startup and25package checks pass. Busy-clear is modeled,
not hardware evidence. Not deployed; M350 remains the lab result. See facts M352.


### M353 - Explicit reset readbacks do not clear immediate busy

111gate2trial reads GRBM_SOFT_RESET0->RLCbit4->0, while disabled RLCbusy stays
set. Helper-62 refuses before PSP firmware load, SSH survives. Closed-gate
PnPrecovery returns111stage61/43presents,count0,sameboot05:20:01,noAC/reboot.
No reset completion or workload acceptance claim. See facts M353. Next review
the first busy transition at retirement GFXHUBflush and RLC/GFXOFF dependencies;
do not repeat unchanged reset trials or infer broader reset masks from busy.


### M354 - Next witness boundary: request/dummy-read/ACK

Source review confirms the GC10.1 dummy request read is already present before
ACK polling. Linux6.18.0 GFXpowergating switch has noGC10.1.3/10.1.4case, so
genericGFXOFF comments do not justify adding a SMUcommand. Existing RLC logs
bracket the whole GFXHUBflush. Next instrument its original request/dummy-read/
ACK boundaries, preserving accesses, polls, semaphore and invalidations. See
facts M354/tlb-retirement-review. No hardware state changed by this review.


### M355 - Retirement TLB observer candidate112 prepared

Scoped GFXHUB retirement callbacks expose existing request-write/dummy-read/
lastACK samples with RLCstate and sequence status. Ordinary wrapper uses null
callback; no invalidation or ownership policy changes. Eight flush scenarios
show equal original accesses with/without observer; ordinaryreplay,22reset,
327startup and25package checks pass. Not deployed. Hardware callback adds
latency/RLCreads and needs a first-start positive control before retirement.
See facts M355/tlb-observer-preparation; warm acceptance remains open.


### M356 - Invalidation ACK succeeds while stopped RLC becomes busy

112firstcontrol passes3cycles/4readbacks. During retirement, PTEunbind leaves
STATUS2 8; the first RLCsnapshot after request00F80001 sees01000008, before
dummyREQread. ACK1/flushrc0/sequencefault0 follow withbusyretained. Instrumented
interval includes observation/time; not proof of one faulting instruction.
See facts M356. Final112display-onlystage61/82presents,gates0,count0,newboot
06:06:14afteronebaselineAC. No warmretry. Review RLC-visible storage/firmware
lifetime and invalidation ordering before changing phases; do not skipflush.


### M357 - Separate CP halt and RLC stop before GTT retirement integration

New shim keep-RLC helper shares the existing fini but omits its final RLCstop;
original wrapper unchanged. Replay verifies haltedCP/SDMA,RLCenabled,CSBretained,
then explicitRLCstop before teardown. Current WindowsCSB isVRAM; AMD permits
VRAMorGTT. No KMDintegration/deployment yet. Next owner-scoped GTTpre-unbind/flush
must retain all backing andCSB until subsequent RLC/PSP/GART retirement, then
avoid duplicate post-stop unbind of already invalidated entries. See facts M357.


### M358 - Owner GTT mapping retirement helper ready for integration

New GpuMemRetireGttMappings unbinds current-owner GTT ranges,flushes bothhubs
once,then commits TranslationsRetired while preserving Bound/Used/Owner/backing.
Later mem_free avoids duplicate invalidation; GpuQuiet remains required for
release.38actual-source checks andWDKbuild pass. No stop caller/deployment yet;
next combine with M357CP/RLCphase split and validate ownership/order. See M358.


### M359 - Candidate113 owner GTT retirement integrated before RLC stop

Completed-CP PnP/startup unwind now halts CP/SDMA retainingRLC, verifies halt,
unbinds current-owner GTT and flushes bothhubs with backing retained, then stops
RLC before PSP/GART retirement. Mappingfailure blocks quiet release. Diagnostic
and preCP paths retain existing behavior.3632stop/327startup/38GTT/25package
checks andWDKbuild pass. Not deployed; hardwarefirststart/content/stop/warm
acceptance remains. See facts M359/pre-rlc-gtt-integration/RESULT.md.


### M360 - Retirement improvement confirmed narrowly; startup still fails

113firstcontrol passes.25ownerGTTretirements/flush occur withRLCenabled andbusy
clear; subsequentRLCstop/PSP/GART/storage/finalrestore keepCNTL0/STATUS2 8.
Warmstart again losesSSH,withbusy alreadyset beforePSP afterGARTinit;11PSP
commands success,lastCP1scheduler-read. MissingpreGARTsnapshot prevents exact
startupcause attribution. Next isolate startupGART invalidation andPSPmemory
visibility dependencies. Final113display-onlystage61/48presents,boot06:29:13,
gates0,count0afteronefailureAC. See factsM360;warmacceptance remainsopen.


### M361 - Startup GART substep observer candidate114

PreGART RLCsnapshot and full-mode ENABLEsubsteps now expose hubconfiguration,
faultdefaults,MMHUBflush andGFXHUBrequest/read/ACK. No ordering/invalidation or
readiness change. PSPbuffers useVRAMMC butsoftwareLOAD requiresGART;hardware
independence remains unproven. Replay/build/327startup/25package pass;notdeployed.
See factsM361/startup-gart-observation. Use current recoveredboot forcontrol if
nofullGPUstartup occurred,then measureonewarmtransition;avoidextraACbaseline.


### M362 - Warm startup transition localized to GFXHUB request interval

Candidate114 first-start/content control passes. Pre-GART,hubconfiguration and
MMHUBflush remain RLCbusy-clear on warm startup; first post-GFXHUBrequest snapshot
is busy despite ACK1/flushsuccess.11PSPcommands succeed; last persisted checkpoint
CP1scheduler-read before SSH loss. Stop improvement repeats with25owner mappings.
One recoveryAC; final114display-onlystage61/53presents,gates0,count0,boot06:44:52.
See factsM362/candidate07114. Next review safe delayed-invalidation or preserved
RLC-context dependencies; do not remove invalidation or reorderPSP solely from
VRAM buffer placement. Warm/cache/DMA/lifetime/performance acceptance remainopen.


### M363 - Configuration extraction and deferred visibility contract

The shim exposes configuration separately; existing full enable still performs
both original flushes. Actual-shim comparison preserves all285writes;configure
alone has no invalidation accesses. Replay andWDKshim compile pass;not integrated
or deployed. See factsM363 and gart-phase-extraction/RESULT.md for source hashes.

Deferring only initialGARTflush is insufficient: IH/GFXsetup GTTbinds each flush
both hubs beforeRLC. Next scope an explicit device-owned pendingGFX phase across
these binds, retain MMvisibility, and complete GFXinvalidation afterRLCstage5 but
beforeCPstage6 or readiness/publication. Gart.Enabled is restoration ownership,
not completion. MM-onlyflush must not clear retirement dirty state or free backing.
RLC CSB currentlyVRAM provides a candidate dependency boundary, not proof of
hardware viability. Verify successful staged control before warm acceptance.


### M364 - Candidate115 pending GFX visibility integrated

Automatic startup now enters an unpublished translation bootstrap with engines
halted. GART and earlybinds flush MM only; pendingGFX anddirty state survive.
Successful RLCstage5 commits bothhubs beforeCPstage6. IncrementalCPsteps and
finaladmission refusepending; boundbacking andstopowners remainretained if the
phasecannotcomplete. Ordinary diagnosticbehavior remainscomplete.
85actual-source checks plusnegativecommitcontrol,328startup/3632stop/38GTT,
WDKbuild/25package pass. See factsM364. Hardwarefirst/warm controls stillrequired;
this source/model result doesnotprove RLCmemoryvisibility orwarmreentry.


### M365 - First staged control passes; warm busy transition moves into PSP interval

Candidate115 firststart/content passes with successful afterRLC translationcommit.
WarmpreGART/afterMMflush/beforePSP remainCNTL0/STATUS2 8, but after11successfulPSP
commands become0/01000008. LastpersistedentryRLCstage5;no completedcommit/CPrecord.
AvoidinginitialGFXrequest is insufficient. Next isolate PSPcommand/RLCstate and
review firmware-reload lifecycle;no unchangedwarmretry. OnefailureAC;final115
stage61/36presents,gates0,count0,boot07:07:39. See factsM365. Broadergoal open.


### M366 - PSP command boundaries prepared for116

LocalAMDPSPresume reloads nonPSPfirmware;11.0.8 disablesautoload. No sourcebasis
for skippingRLCreload merelybecause coldload succeeded.116adds RLCsnapshots
before/afterringcreate andaftereachcommand,without changing protocol orordering.
WDK/25package/PSPmodel pass;newKMDobservations notyet hardwarevalidated. SeeM366.
Nextfirst/warmcontrol identifies earliestchangedcommandinterval. Lab115display-only
boot07:07:39 remainslastverifiedstate. BroaderM9goal stillopen.


### M367 - PSP command2 SDMA0 bounds first warm busy transition

116firstcontrolpasses3cycles/4readbacks. Coldcommands1-10retainRLC0/8;
RLCload11temporarilybusy withCNTL1,thenstage5commit1/8. Warmringcreate/TMRremain
0/8,SDMA0loadcommand2first0/01000008,throughremainingcommands. Stage5commit
reaches1/01004008;lastCP1scheduler-read. Busy aloneisnotfailureproof.
NextreviewSDMA0-start/RLCdependencies;existingSDMAfini alreadydisablesRB/IB.
OnefailureAC;final116stage61/39presents,gates0,count0,boot07:18:37. SeeM367.
Warm/broaderM9acceptance remainsopen;no unchangedwarmretry.


### M368 - Distinguish ordinary SDMA fini from reset preparation

LocalAMDstop_queue adds pairedRLCsafe-mode,freeze/idlewitness,F32halt andUTC_L1
disable;restore_queue unfreezes before ringresume. Currentordinaryfini disables
queues/contextswitch andhalts,butdoesnotperformthisreset-preparation phase.
Next evaluate completequiescence/unfreezecontract withbacking retained,notan
isolatedresetbit orfirmwareskip. SeeM368/sourcehashes. Causality andwarmviability
remainunproven; nohardwarechanged. v6.18instance-maskshift discrepancyrecorded,
shim's boundedrealinstance loop preserved andcommentcorrected.


### M369 - SDMA quiescence primitive ready for lifecycle integration

Uncalled shim quiesce/unfreeze functions adapt AMDstop_queue/restore_queue register
fragments.6both-instance models verify freeze/idle,halt/cacheoff order andpaired
unfreeze;ordinary354+35replay andWDKshimcompile pass. Caller must supplyactual
RLCsafe-mode scope,serialization,retainedbacking andring/translationrestoration.
NoKMDcaller/deployment yet. SeeM369;nextresolveRLCscope andfreezeownership across
PSPreload beforehardware. Installed116unchanged;warm/broaderM9stillopen.


### M370 - Candidate117 scoped quiescence before mapping retirement

AfterCP/SDMAhaltwithRLCretained,explicitRLCsafe-mode entryACK precedesbothSDMA
freeze/idle/halt/UTCoff phases. Unfreezewhilehalted/queuesoff/cacheoff,readbacks,
pairedexit,thenownerGTTretirement/RLCstop. FREEZEneednotpersistacrossPSPreload.
Error/fault blocksquietrelease.4scope+6instance models and3634stop/328startup/
38GTT/85bootstrap/25packagepass. Final117packagein07117-final,notdeployed.
SeeM370;nextfirstcontrol/instrumentedstop,requirequiescencerc0beforeonewarmtrial.
Installed116remainslastverified;hardwarewarmandbroaderM9acceptance stillopen.


### M371 - Candidate117 hardware result

First content control and RLC-scoped SDMA quiescence passed. Stop retired25 owner
GTT mappings with RLC clear, but one warm trial repeated M367: first observed
busy after PSP SDMA0 load, all11 commands successful, last CP1 scheduler-read.
Quiescence alone is insufficient in this trial. See M371 and candidate07117/RESULT.md.
One recovery AC restored boot07:47:33; final117 display-only stage61/gates0/count0.
Next review the actual AMD per-engine reset and restoration contract; no unchanged
warm retry, no hardware reset success claimed. Broader M9 remains open.


### M372 - Per-engine reset primitive and composition requirements

AMD soft_reset_engine is extracted with exact register/delay order and tested for
both engines. It is uncalled by KMD; installed117 is unchanged. Source reset runs
after stop_queue safe-mode exit and before a new restore_queue scope. Return0
alone does not prove reset. Next composition must retain backing, require stop
success and establish post-reset halt/queue/cache state before retirement, or
restore rings/translations and test them before admission. See M372 evidence.


### M373-M374 - SDMA reset integration and hardware result

Candidate118 adds reset between two RLC scopes, then reestablishes post-reset
HALT/queue-off/cache-off before retirement. Host models and build pass. Hardware
first content control passes; both reset bits assert/release and post-quiescence
returns0/fault0. Warm still loses SSH after SDMA0-post RLCbusy; last persisted
point is entering RLC stage5, not CP1 in this trial. See M374. Final118 recovered
display-only, boot08:06:02, stage61/gates0/count0. Do not repeat unchanged118;
mask readback alone is not internal-reset proof. Warm and broader M9 remain open.


### M376 - Candidate119 hardware content and post-workload incident

Installed119 passes64KiB and1GiB3-cycle/4-read content controls in one session.
FinalGFX4100/4100,paging170416/170416,noerrors/TDR;30reserved captures/0heap.
After successful script completion SSH becomes unavailable before collection,
without warm restart or device stop. Cause unknown. OneAC recovery returns
boot08:30:00; final119stage61/gates0/count0. Concurrent client trial not started.
See M376; content correctness does not establish post-workload session stability.


### M377 - Concurrent clients and post-completion observations

Two11964MiB clients pass all byte oracles, with12reserved/0heap.60s polled
observation and a separate60s interval with only OS time heartbeats both pass;
finalGFX512/512,paging29899/29899,noerrors/TDR. M376 loss was not reproduced by
this changed workload; cause still unknown. Same-context arena overlap and
unbounded idle stability are not proved. Full119 session retained, boot08:30:00,
stage50/count0, GPU execution gates enabled; Full gate0 is one-shot, not display-only.
See M377. No reboot/AC/warm retry in this trial.

### M389 - Reset engine attribution and safe-mode policy

Facts M389: E29 fresh control pinned to physical SDMA1 passes; mask3 restored. Linux reset window omits RLC safe-mode commands, consistent with its zero clock-gating policy. Windows initializes the same zero flags but its helper forces commands. Correct that source-policy mismatch, retain conditional ACK/pairing coverage, and test a new candidate. This is not proof of warm-reload causation or full M9 acceptance. See [comparison](../../evidence/linux/2026-09-24-E29-sdma-reset/comparison/RESULT.md).

### M390 - Conditional RLC scope implemented locally

[Candidate121](../../evidence/windows/2026-09-24-E27-m9-recovery/rlc-cg-policy/RESULT.md) skips requests for zero/unrelated cg_flags and retains ACK/paired exit for relevant flags. Eight reset-model scenarios, full GFX replay, WDK and package checks pass. No deployment or runtime success yet. Linux E29 remains active; next return to Windows for121 first-load and then warm reentry controls.

### M391 - Candidate121 runtime result

[Hardware](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07121/RESULT.md): first-load64KiB3cycle GPU readback passes; capture peaks1plan/1648bytes. Stop reset/quiescence/retirement returns0. Warm still losesSSH afterSDMA0firmware marksRLC_BUSY; all11PSPsuccess, last persisted stage5entry. OneAC recovers121display-only. Policy correction does not close reentry; investigate firmware/RLC lifetime instead of repeating121.

### M392 - Distinguish RLC resume from deferred bootstrap completion

[Source review](../../evidence/windows/2026-09-24-E27-m9-recovery/rlc-stage-boundary-review/RESULT.md) shows the stage5 wrapper also completes pending GFXHUB visibility before logging return. M391 cannot distinguish these boundaries. PSP teardown ordering matches source at the reviewed command boundaries; no early-free defect established. Next add PASSIVE-safe persisted boundaries with the existing CP1 lock approach, preserving MMIO order. No121 rerun or new deployment.

### M393 - RLC/visibility boundaries prepared

[Candidate122](../../evidence/windows/2026-09-24-E27-m9-recovery/rlc-boundary-checkpoints/RESULT.md) persists before/after RLC resume and pending GFX commit using a paired PASSIVE-safe lock path.220source-harness checks pass; omitted callbacks produce4expected assertion failures. WDK/package pass, undeployed. Next hardware first-load control then one warm trial;121 remains recovered display-only.

### M394 - RLC call returns in failed warm trial

[Candidate122](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07122/RESULT.md) passes first-start checkpoints and64KiB GPUreadback. Warm persists after-rlc-resume and before-gfx-visibility, narrowing failure to deferred visibility interval, including observer/MMHUB/publication. Exact access and safe fix remain unproved. OneAC recovery;122display-only, no repeat. Preserve visibility contract while separating substeps.

### M395 - Visibility access checkpoints prepared

[Candidate123](../../evidence/windows/2026-09-24-E27-m9-recovery/visibility-access-checkpoints/RESULT.md) persists existing post-request/read/ACK messages before RLC observer reads and marks observer return, MMHUB and publication separately. No extra MMIO/skipped flush.228host checks and WDK/package pass; undeployed. Next positive control then one changed warm trial.

### M396 - Warm invalidate request returns

[Candidate123](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07123/RESULT.md)
passes first-load content. Warm persists post-request but no observer return.
Next remove diagnostic MMIO interleaved in the flush, retaining all required
visibility operations. One AC recovery; Windows123 display-only, boot11:01:13.


### M397 - In-flight diagnostic reads removed

[Candidate124](../../evidence/windows/2026-09-24-E27-m9-recovery/tlb-observer-no-mmio/RESULT.md)
removes RLC/GRBM reads between invalidate accesses. Required flush and post-flush
tracing remain.230host checks pass; old-callback mutation fails18assertions;
WDK/package pass. Undeployed, next first-load control then one warm trial.


### M398 - Candidate124 warm failure persists

[Hardware trial](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07124/RESULT.md)
passes first-load content and stop. Warm persists the pure post-request observer
checkpoint, not the following dummy-read checkpoint. Removing diagnostic MMIO
does not fix reentry. OneAC recovery returns124display-onlyboot11:17:48.
Next review GFXHUB/RLC/firmware-reload state; keep required visibility accesses.


### M399 - Full-stop GART state aligned with AMD retirement

[Candidate125](../../evidence/windows/2026-09-24-E27-m9-recovery/full-stop-gart-state/RESULT.md)
keeps contexts/cache disabled after successful full-WDDM retirement. Firmware
snapshot restoration remains for display-only diagnostics. Required flushes
and ownership ordering stay intact.3644stop/230bootstrap checks pass; old replay
mutation fails2checks, WDK/package pass. Not deployed; warm fix unproved.
Next positive first start, verify stop policy, then one warm trial.


### M400 - Full-stop policy runs; warm reentry remains unresolved

[Candidate125](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07125/RESULT.md)
passes first-load GPU content and new stop policy. Warm failure remains after
the post-request observer checkpoint, before dummy-read confirmation.
OneAC recovery restores125display-onlyboot11:30:53. Next investigate upstream
firmware/reset-state dependencies; no unchanged retry or invalidation omission.


### M401 - Observe CP/GFX busy classification input

[Candidate126](../../evidence/windows/2026-09-24-E27-m9-recovery/grbm-reload-observation/RESULT.md)
adds raw GRBM_STATUS and read status at existing RLC checkpoints. No reset or
in-flush MMIO is added. WDK/package pass, undeployed. Compare first-load/stop/
warm samples against original AMD masks before choosing a reset hypothesis.


### M402 - No CP/GFX busy discriminator in new samples

[Candidate126](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07126/RESULT.md)
passes first-content/stop but still fails warm. GRBM_STATUS0x3028/read0 is
identical across captured phases, with no busy bits used for CP/GFX reset.
RSMUpending is present in the working control too. OneAC recovers126display-only.
Next review the existing pp_gfxoff=false RLC-SMU policy branch as a hypothesis;
do not broaden reset masks from these samples or omit required visibility.


### M403 - Source-checked no-GFXOFF startup policy

[Candidate127](../../evidence/windows/2026-09-24-E27-m9-recovery/rlc-no-gfxoff-policy/RESULT.md)
selects pp_gfxoff=false for full WDDM, retaining true for diagnostic replay.
Actual AMD/shim helper traces match4scenarios; reversed branch fails4.
Full replay and WDK/package pass. Undeployed, no handshake-cause claim.
Next first-load content control, stop, one warm trial and post-warm content.


### M404 - Warm start and post-warm content pass with no-GFXOFF policy

[Candidate127](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07127/RESULT.md)
completes one stop/warm sequence without OS/AC reset. Post-warm64KiB/1GiB GPU
readbacks,8shader CPU hashes and both Linux-reference model outputs pass.
FinalGFX6454/6454,paging197311/197311,noerrors/TDR,74reserved/0heap.
Retain initialized127session boot11:44:14,count0. Repeated lifecycle, resource,
cache/lifetime and performance requirements remain open; full M9 not achieved.



### M422 - Native OS paging positive path

[Candidate131](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07131-native-os-paging/RESULT.md) executes admitted ordinary copy/fill from OS DMA
through VMID2 and checks GPU permissions/CPU backing before publication.
One PnP transition preserves OS/DWM; shader, model and1GiB mixed-path content
controls pass. The1GiB transfer route still uses physical captures, so resource
guarantees and all remaining acceptance-index gates stay open.


### M423 - Fragmented native DMA admission fixed

[Candidate132](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07132-exact-dma-disjoint/RESULT.md) checks actual pages when coarse DMA/data bounds
overlap. It admits1GiB residency transfers natively, with0capture plans in
64MiB/model/1GiB controls, correct full GPU readbacks and noTDR. This supersedes
M422's measured large-transfer exclusion. Preallocated arenas and physical
fallback still exist; complete resource/cache/lifetime/startup gates stay open.

## M424 - Queue storage preparation

The [DMA-private queue format](../design/paging-queue-storage.md) reserves per-range
job slots and passes host/WDK tests. Runtime builders and queue still use the old
format and per-submission nonpaged allocation/copy. Integrating admission and
retirement-before-OS-reuse is the next resource step; capture fallback remains
separate. Installed M423132 is unchanged; full M9 remains open.

## M425 - Borrowed queue integration, builders pending

The actual queue consumes M424 slots without allocation/copy and releases them
before completion publication.590 host checks pass; late-release mutation
fails91; full WDK and875868 builder checks pass. Builders still emit legacy
records, so live submissions still allocate. M423132 remains installed. Next
migrate all private budgets/headers/alignment and test builder-to-queue paths.
See [queue design and evidence](../design/paging-queue-storage.md). OS lifetime,
preemption/cancellation and the remaining full M9 gates stay open.

## M426 - OS-private queue deployed

All builders reserve queued records and the submit allocator/copy fallback is
removed. Candidate133 passes907971 builder/711 queue checks and actual64MiB/1GiB,
8shader/twoE14model controls.30673 borrowed admissions equal SDMA completions,
GFX6706/6706,zero errors/noTDR. Same OS/DWM; one PnP only. Capture fallback,
OS cancellation/preemption/generation, cache/PFN and cold/power/performance
requirements stay open. See [M426 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07133-os-private-queue/RESULT.md).

## M427 - OS-boot gate failure and successful PnP control

[Pagefile/startup evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07133-pagefile-osboot/RESULT.md)
records32GiB pagefile activation and a failed full-table OS-boot selection:
one-shot persistence returns0xC000014D before GPU startup. The same133 binary
then passes one PnP full start and64MiB three-cycle/four-readback control without
another OS/DWM restart. Guard durability is not weakened. Investigate table
selection at early boot separately from engine readiness; this result does not
close OS/cold-power startup or the other full M9 acceptance gates.

## M428 - Complete6GiB working-set control

[Complete-set evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/resident-set-6g/RESULT.md)
retains six1GiB allocations simultaneously.103 complete-set snapshots report
6GiB GPU-memory,0shared/nonresident; full GPU readback validates all words with
6144fences and distinct member patterns.64MiB set and legacy-cycle controls pass.
No reset; finalGFX6784/6784,paging16614/16614,noTDR,zero captures. This is stronger
capacity/content evidence, not12GiB acceptance or continuous physical tracking.
A single6GiB allocation is rejected at creation, cause unresolved. OS-boot policy,
resource/cache/lifetime and matched-performance requirements remain open.

## M429 - Clock readiness prerequisite prepared

[AMD clock policy](../../evidence/windows/2026-09-24-E27-m9-recovery/startup-clock-policy/RESULT.md)
passes192host checks and WDK object compile; readback/exclusion mutations fail.
No KMD backend or startup wiring yet. [Ownership design](../design/startup-clock-ownership.md)
requires one mailbox owner across bc250rd/KMD before hardware use. The existing
user-mode underclock task cannot establish clock-before-engine ordering at boot;
persistent table selection alone is not readiness. Lab remains M428, with no
new boot, mode2 policy or hardware experiment. All remaining acceptance gates stay open.

## M432 - DCN/VidPn hardware control

[Candidate134](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07134-dcn-publication/RESULT.md)
passes one PnP update with retained OS/DWM,533 hardware flips without ACK
timeouts/refusals, D3D content,64MiB full GPU readbacks and shader/model references.
GFX2610/2610, SDMA6051/6051, no TDR. The earlier full-WDDM dispatcher refuses the
diagnostic flip before M431's inner guard; the script's wrong-layer expectation
is recorded as an observer failure. This run does not close cold/OS-boot or the
other remaining M9 gates. M429 native SMU ownership/backend/integration remains
pending. Authoritative deployment is workspace STATE.md.

## M441-M443 - Native owner deployed; delegated fixes pending

[M441](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07136-native-smu/RESULT.md)
completes legacy-writer retirement, PnP activation and verified clock readiness
before engines in0.7.136.1. Same-session content/display controls pass. Cold boot,
full D0 restoration, partial-failure/lifetime acceptance remain open.
[M442](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-rlc-pte-review/RESULT.md)
and [M443](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-display-observation/RESULT.md)
are later source-only fixes/reviews, requiring a distinct next candidate.

## M455 - Durable full-table admission and OS-restart control

Full144 requires successful counter read/write/flush before POST/MMIO. Persistent
policy2 then passes one OS restart with automatic clock/engine initialization
and postboot64MiB, eight shader and two model content controls. One-shot policy1
still refuses failed closure, with separate write/flush diagnostics. The old
M427 combined error did not identify which operation failed. See
[implementation and runtime evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/durable-osboot144/RESULT.md).
AC-cold entry, full power resume and automatic full-table health confirmation
remain open. The current monitor waits for DDO-only stage61; this run used
content-controlled confirmation followed by a managed registry flush whose
native result was not exposed. M456 replaces that interpretation with a checked
RegFlushKey success on the same live144 session; see
[checked confirmation](../../evidence/windows/2026-09-25-E27-m9-recovery/checked-confirmation144/RESULT.md).

## M457 - Automatic successful-start confirmation

Candidate145 implements the typed identity/presentation handshake described in
[confirmation contract](wddm-start-confirmation.md). A genuine warmPnP start is
automatically confirmed after60s observed progress; the owner confirms physical
image/input and64MiB GPU controls pass with the same OS/DWM.
[Evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/automatic-confirmation145/RESULT.md). AC-cold, repeated production boots, supported
power resume and the other M9 audit contracts remain open.

## M458 - One AC-cold entry with automatic confirmation and content controls

Unchanged145 starts after normal shutdown and a verified31.477s AC-off interval.
The continuous startup recorder captures pending health, advancing primaries,
automatic confirmation and checked kernel persistence.64MiB readback, eight
shader hashes and both E14 model outputs pass without manual initialization.
[Evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/ac-cold145/RESULT.md).
The owner confirms smooth physical output for the new boot
([supplement](../../evidence/windows/2026-09-25-E27-m9-recovery/ac-cold145-owner-feedback/REPORT.md)). Power resume, preemption/recovery
and remaining memory/lifetime contracts are not closed. The attached
[retained-owner resume review](../../evidence/windows/2026-09-25-E27-m9-recovery/ac-cold145/power-resume-source-review.md)
is a source-derived implementation plan, not implemented resume support.

## M459 - Offline SMU capability metadata prerequisite

The SMU version snapshot now survives hardware stop and failed restart; cached
queries neither access the mailbox nor wait for its transaction lock. Concurrent
actual-source host controls and a regression mutation discriminate this behavior;
WDK build passes. This is undeployed source work, not hardware power resume.
[Evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/retained-smu-metadata/RESULT.md).
Retained WDDM objects/queues, hardware restoration and PSP/capability lifetime
remain required before wiring the resume coordinator into SetPowerState.

## M460 - Retained WDDM software boundary

WddmSuspendRetained/WddmResumeRetained now preserve software ownership and OS
fence history while closing/joining private work and restoring requested
notifications. Actual-source host191/0, targeted mutations and WDK build pass.
[Evidence and limits](../../evidence/windows/2026-09-25-E27-m9-recovery/retained-wddm-power/RESULT.md).
These functions are not wired to SetPowerState. Hardware suspend/restore,
private firmware/backing, IH/diagnostic lifetime, display and health-epoch
restoration remain required; no runtime resume or full M9 acceptance.

## M461 - Retained IH hardware boundary

IH halt and storage teardown are separated. Retained restore prepares disabled
hardware before resetting writeback pointers and enabling the existing synchronized
consumer on the same backing. Actual-source host83/0, two regression mutations
and WDK build pass. [Evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/retained-ih-power/RESULT.md).
No deployment or power DDI wiring. Retained GART, firmware, engines and the complete
coordinator/display/health path remain required before real resume acceptance.

## M462 - Retained private GTT reconstruction

GpuMemRebuildRetainedGtt reconstructs private mappings from retained backing,
excludes retired entries, preserves the Windows-owned tail and leaves TLB work
pending. Actual-source host22/0, targeted mutations and WDK build pass.
[Evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/retained-gtt-rebuild/RESULT.md).
Unwired and undeployed: retained hub configuration, firmware/private VRAM,
engine restoration and the complete resume coordinator still remain required.
