# M9 acceptance status

Reviewed 2026-09-25 through M471. This is the requirement index for the
[DMA contract audit](m9-dma-contract-audit.md) and
[WDDM startup plan](../design/wddm-startup.md). M9 is not complete.
[Facts](../facts.md) and their immutable artifacts are the evidence; source,
host tests and a successful workload establish different scopes of acceptance.

## Current baseline

**Current warm baseline (M470-M471):** KMD147 plus E26R v2 cached shared-surface
intent and bounded UMD diagnostics. Matched4MiB CPU reads improve from~156ms
to~1.25ms with full contents checked. Shared/pixel,64MiB GPU,8shader and both
AI reference controls pass. A short Winlogon cursor trace reaches176 presents
in3s with zero glitch events after removing per-entrypoint diagnostic I/O.
The earlier cache-only version still stuttered during owner-observed animation;
physical feedback and a realS4 on the final quiet UMD remain pending. These
results do not replace the broader requirement table or establish hardware D3D.
[Cache controls](../../evidence/windows/2026-09-25-E27-m9-recovery/shared-cpu-cache147/RESULT.md),
[quiet UMD controls](../../evidence/windows/2026-09-25-E27-m9-recovery/bounded-umd-diagnostics147/RESULT.md).


**Current cold-start/content control (M458):** unchanged full145 starts after
normal shutdown and one verified31.477s AC-off interval. A continuous cached
health recorder observes pending guard1/flags7, increasing completed primaries,
and automatic monitor confirmation after60s progress. Guard0/flags15 and checked
kernel flush success are retained. No manual GPU initialization, PnP retry or
DWM restart.64MiB full GPU readback, eight shader hashes and both E14 model
outputs pass; GFX2610/2610,paging6047/6047,noTDR.
[Cold145 evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/ac-cold145/RESULT.md).
This is one measured cold entry, not power resume, repeated-boot acceptance,
soak or complete M9. The owner subsequently confirms a visible desktop and
smooth cursor/overlay for this cold boot
([feedback supplement](../../evidence/windows/2026-09-25-E27-m9-recovery/ac-cold145-owner-feedback/REPORT.md)).

**Previous OS-start/content control (M455):** full 144 starts automatically after
one Windows restart with persistent policy2 and a durable start-budget flush
before hardware admission. Native clock preparation precedes engine startup.
Postboot 64 MiB paging/readback, eight shader hashes and both E14 outputs pass;
GFX2610/2610 and paging6037/6037 complete without TDR. No postboot CLI RUN,
PnP retry or DWM restart. This is OS-restart evidence, not AC-cold or resume.
[144 evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/durable-osboot144/RESULT.md).
This144 run used explicit confirmation after
content controls. M456 corrects the earlier unchecked managed flush: the new
CLI checks native persistence and succeeds without restarting Windows/DWM.
[Checked confirmation](../../evidence/windows/2026-09-25-E27-m9-recovery/checked-confirmation144/RESULT.md).
The owner also confirms correct physical image and cursor after this restart
([feedback](../../evidence/windows/2026-09-25-E27-m9-recovery/durable-osboot144-owner-feedback/REPORT.md)).

**Current display recovery (M453):** KMD 143 restores physical output by making
VidPn and DescribeAllocation use the same cached 59.950171 Hz timing. Current
ETW mode-change errors fall from 41 to zero; hardware blank control is zero.
The owner confirms desktop visibility and smooth cursor after observation ends.
M451/M452's blank-screen regression is resolved for this inherited mode; brief
stutter during HardwareAccess diagnostics was not isolated as a persistent defect.
[143 evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/shared-refresh143/RESULT.md).
This does not repeat the broader compute/memory acceptance below or prove a soak.

M441 deployed KMD 0.7.136.1 with native SMU ownership after retiring the old
writer. The clock is confirmed at 1000 MHz / VID116 before GART, PSP, IH and GFX.
D3D content, 64 MiB paging/readback, eight shader hashes and both E14 model
outputs pass. Windows and DWM survive the warm PnP transition. This does not
prove cold boot, upward-voltage settling, full power resume or full M9.
[Candidate136 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07136-native-smu/RESULT.md).
Exact installed identities and rollback artifacts live in workspace STATE.md.

The desktop retains Mesa main f9a2d34a / LLVM 23.1.2 llvmpipe CPU rendering with
hardware DCN flips. Vulkan compute retains Mesa f333dd6d RADV/ACO. Preserve
those module identities and display gates during M9 tests. Older scripts that
close display gates or restore the stub alone are not suitable for this profile.
The owner confirms smooth image/input on 143; this is not timed soak acceptance.

M442-M443 source fixes add supported RLC-header validation, hardware raster and
actual-scanout flip observation, with partial pending-vblank reporting. M444's
1024-query baseline on136 still reports blank/line0 throughout. M445 adds actual
loaded/runtime firmware metadata; M446 adds POST restoration before CPU display
output and handover; M447 adds bounded PTE attempt observations and reviews cache
aliases. These changes are included in 143. M451/M453 add raster, inherited timing and
physical display evidence; bugcheck/handover and other scopes remain open.
M448 defines the graphics-specific IOMMU migration prerequisites; implementation
is absent. Refer to the matching [fact entries](../facts.md), not a shared
working-tree version number, to identify each source snapshot.

## Requirement-by-requirement acceptance

| Requirement | Existing evidence | Remaining work / required proof |
|---|---|---|
| Shared initialization, prepared firmware, phase ownership and unwind | M233-M241 extracted ownership/failure controls and automatic startup integration; M441 clock-before-engine witness | Containment across failed hardware retirement and device removal/recreation. Positive starts do not prove partial-failure cleanup. |
| Ready engines/queues before first supported OS work | M250/M251 early OS paging; M426 prepared private queue; M441 automatic full136 startup and post-start content | Current cold-start delivery and every advertised operation must meet the same readiness invariant. No production CLI RUN dependency or empty-success fallback. |
| Cold boot and durable table selection | M455 checked OS admission; M457 warm confirmation; M458 one AC-cold entry with continuously observed automatic confirmation | Repeated production-boot coverage and supported power transitions. M427 did not distinguish one-shot write from flush failure. |
| Repeat start and power resume | Repeated warm PnP controls through M441; M399/M403 ownership and no-GFXOFF corrections | M463 integrates retained hardware owners and passes one S4 with original handles and full64MiB content. Owner reports postresume sign-in input stutter; stable unlocked desktop, repeated transitions and other supported D-states remain open. |
| Paging DmaSize and multipass | M167-M168 actual builders/old-code regression; subsequent real paging and M426 queue integration | Match every supported operation's remaining-capacity, pointer, progress and private-record bounds. Insufficient DMA buffer must mean capacity, never readiness waiting. |
| System/fragmented transfers and GPU table ordering | Source work through M311; M419 VMID2 remap; M423 exact fragmented DMA admission; M426 live native paging | Close mapping/PFN lifetime and captured dependency shapes beyond exercised workloads, keeping queued versus executed table state distinct. |
| Physical/private submission ranges and virtual payload | M169 per-buffer records/ranges; M171 virtual refusal ordering; M426 OS-private queue | Confirm actual OS ownership through cancellation/preemption and device generations. Do not apply virtual DDI error policy blindly to physical submission. |
| OS TLB and PTE publication | Immediate CPU_VIRTUAL initialization and GPU_PHYSICAL construction tests; live GPU controls | Complete per-process/root/engine ordering, alias visibility and ownership acceptance. New M447 counters measure encoding attempts, not retirement. |
| Alias semantics | Equal-offset cycles, compatible-direction unequal dependencies and repeated source reads have host controls; M423 normal native admission passes | Conflicting-direction unequal intervals and repeated destinations remain unsupported. Resolve their actual contract/demand; never claim arbitrary snapshot-copy support from ordinary disjoint tests. |
| Bounded builder resources and OS-facing outcomes | Prepared context reservations; M426 removes queue submission allocation/copy; tested large native transfers use zero captures | Physical/table/alias capture fallback still includes allocating paths. Establish resource bounds for supported work without arbitrary caps, empty success or infinite insufficient-buffer retries. |
| CPU aperture/cache attributes and GPU coherence | Separate table storage; POST selector fixed; M447 selector2072/0 and cache review; shader/eviction controls | Retained versus borrowed Windows mappings need compatible effective attributes or a design eliminating aliases. M317 physical cache query failed; its output is not a valid cache type. BD-021 local coherence requests still need observation. |
| Context/capture/queue lifetime and honest completion | Host admission/retirement controls; M377 concurrent clients; M426 admissions/completions agree | Same-context interleaving, actual OS cancellation, PFN retention and device-generation transitions. Elapsed time must never stand in for hardware halt or fence retirement. |
| Preemption and hardware recovery | Per-engine reference/reset investigations and source policy recorded in startup plan | Forced CSA preemption/restoration, companion roots, real halt/reset/restart and accepted-work disposition remain unproved. Refusing reset honestly is not successful recovery. |
| Present packet contract | D3D shared/pixel/60-present controls pass on136 | Resolve the reserved pAllocationInfo usage against the supported WDDM2 Present contract; keep Present's input DmaSize distinct from paging's in/out size. |
| Scanout completion, timing and handover | M443 source observation; M444 bad136 raster baseline; M446 bounded POST restore controls | Raw hardware raster/latch/timing proof, visibility and handover controls. Bugcheck framebuffer restoration does not implement full GPU cancellation/reset. Ambiguous vsync observations remain deferred. |
| Firmware/clock ownership and truthful caps | M441 one native owner and retired legacy writer; M445 cached metadata source controls | M451 runtime firmware query validated; M458 cold entry observed. Supported resume and stop/lifetime validation remain. |
| Pressure and actual GPU residency | M423 full1GiB readback; M428 six simultaneous1GiB members, sampled residency and full GPU reads | Wider pressure/concurrency and the owner's12GiB residency request. Current8GiB carve cannot meet it; projected12GiB carve leaves about11.5864GiB app capacity. Firmware modification is a separate authorized/recoverable operation. |
| Equivalent Windows/Linux performance | M415 one paired Mesa comparison; M423 paging improvement over131; earlier Linux/Windows baselines | Matched models, clocks, configuration, repetitions and correctness on the accepted implementation. Windows superiority has not been demonstrated. |

## Next actions

1. Validate post-S4 sign-in interaction on147 with the cached-shared, quiet UMD;
   the short active Winlogon baseline is improved, while animation/resume acceptance remains open. Retained power coordination now preserves OS objects, mappings
   and native clock/engine ownership; full StopDevice/StartDevice is not suitable.
   [Source review](../../evidence/windows/2026-09-25-E27-m9-recovery/ac-cold145/power-resume-source-review.md).
2. Validate the supported transition with post-resume content and durable health
   evidence. M458 closes the first cold-entry observation gap; do not repeat
   boots merely to increase the count. S4 was disabled during M458; M463 enabled and exercised it. Firmware reports
   no S1/S2/S3/S0ix support.
3. Resolve the remaining supported paging resource/status, alias, cache and OS
   lifetime requirements. Test successful paths first; use targeted failures only
   where they establish a real ownership or contract invariant.
4. Establish preemption/recovery and partial-failure generation acceptance without
   freeing storage while hardware can still reference it.
5. Measure equivalent Linux/Windows performance, profile costs and optimize while
   retaining data correctness. No performance or capacity aspiration is a fact.

The [preserved historical index](m9-acceptance-history-through-m447.md) contains
prior progress narratives and old baseline labels. It is history, not current
acceptance. Detailed historical implementation entries in the audit/startup plan
likewise do not override this requirement table or the deployment state.

## Historical prerequisites M459-M462

The following checkpoints describe their original source-only scope. M463
integrated and deployed them on146 and passed one retained-resource S4 control.
Their earlier unwired/undeployed labels are historical, not the current state.
Post-S4 desktop stability remains open after the reported input stutter.

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

## M463 retained S4 control

[Evidence](../../evidence/windows/2026-09-25-E27-m9-recovery/retained-s4-146/RESULT.md): one actual S4 preserves the original64MiB probe process and resources; postresume shaders/models pass. Later owner feedback reports input stutter at sign-in. The owner cancelled remote login; recovery cold146 restored a smooth desktop. A passing data test does not establish stable desktop resume.

## M464 diagnostic isolation

[Monitor control](../../evidence/windows/2026-09-25-E27-m9-recovery/monitor-summary-pause/LIVE.md) pauses only synchronized LOG_SUMMARY requests. The live12s control produces no new summary while completed presentation progresses. This prepares a post-S4 comparison; it does not prove the stutter cause. UDP telemetry transport would not remove the synchronized acquisition cost.

## M465 paging preemption boundary

[Source/host proof](../../evidence/windows/2026-09-25-E27-m9-recovery/paging-preemption-boundary/ROOT-VALIDATION.md): the driver now stops issuing queued paging work at the advertised DMA-buffer boundary, then lets VidSch replay unexecuted work with original IDs. Real scheduler preemption, hardware reset/recovery and any separately planned finer-grained state restoration remain unproved. The development build is not deployed.

## M466 second S4 isolation

[Second trial](../../evidence/windows/2026-09-25-E27-m9-recovery/s4-isolation146/RESULT.md) passes retained contents, shaders and model references with full summary polling paused, but the owner still reports regular cursor stutter. Polling pause alone is not a cure. The [ETW analysis](../../evidence/windows/2026-09-25-E27-m9-recovery/s4-isolation146-etw/ETW-ANALYSIS.md) finds regular vsync, sub-millisecond ISR/DPC maxima and roughly35ms CPU composition, but no captured movement oracle. Long wait-for-work intervals cannot be classified as lost input. A matched cold/post-S4 input-to-present control remains required; stable interactive resume and M9 acceptance remain open.

## M467 zero-capacity capture admission

[Source/host and WDK validation](../../evidence/windows/2026-09-25-E27-m9-recovery/zero-capacity-capture/RESULT.md): fresh capture is not retained when the output buffer has zero usable capacity. Existing byte-oracle, resume/interleaving and M465 preemption controls pass. Undeployed; this does not close reservation exhaustion for unfinished nonempty requests.

## M468 paired S4 input and CPU-render timing

[Matched traces](../../evidence/windows/2026-09-25-E27-m9-recovery/paired-s4-input146/RESULT.md) locate a measured long Present in llvmpipe CPU rendering before the kernel flip. DWM restart only slightly improves owner-observed stutter. Input coordinates and short DWM cursor work can be healthy while heavier frames stall. Investigate shared CPU-read backing/cache policy without claiming it is already the cause.

## M469 single-instance SDMA primitive

[Source/host validation](../../evidence/windows/2026-09-25-E27-m9-recovery/sdma-single-recovery/RESULT.md) establishes isolated register/transport sequence and preserves ordinary startup replay. No production caller or deployment. A hardware content/fence oracle and full OS ownership/reset coordination remain required; paging TDR can escalate to adapter-wide recovery.

## M470 shared CPU cache and remaining composition costs

[Paired147 controls](../../evidence/windows/2026-09-25-E27-m9-recovery/shared-cpu-cache147/RESULT.md) validate E26R v2 CPU-read backing, shared D3D contents and compute regressions. M465/M467 source changes are now deployed; M469 is included but remains unwired. Their independent runtime acceptance remains open. Owner still reports animation stutter; new trace points to synchronous UMD logging and CPU surface copies. Default/Winlogon scene mismatch forbids attributing all frame differences to cache. No S4 cure or M9 completion.

## M471 bounded renderer diagnostics

[Matched Winlogon capture](../../evidence/windows/2026-09-25-E27-m9-recovery/bounded-umd-diagnostics147/RESULT.md) reaches176 presents/3s with zero glitch events after making verbose UMD tracing opt-in. Shared/pixel contents pass. This is a short active baseline, not renewed S4 or stable-resume acceptance. Automatic startup confirmation is still pending in the mostly idle secure desktop. No primary-shadow architecture was deployed.

The [M471 exact-stack analysis](../../evidence/windows/2026-09-25-E27-m9-recovery/bounded-umd-diagnostics147-etw/RESULT.md) reduces DebugPrintf sample share52.8% to2.84%; remaining logging samples are retained frame metrics.
