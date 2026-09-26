# M9 acceptance status

Reviewed 2026-09-24 through M447. This is the current acceptance index for the DMA audit and startup plan; historical progress entries do not override these gates. Facts and immutable evidence remain in [facts.md](../facts.md).

## Current hardware and source state

M441 deploys [candidate136 with native SMU ownership](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07136-native-smu/RESULT.md).
The legacy writer is unloaded and replaced before activation; its old SMU IOCTL
now refuses. Startup verifies 1000 MHz / VID116 before GART, PSP, IH and GFX.
Typed telemetry/SET, D3D,64MiB readbacks,8 shaders and both E14 outputs pass.
GFX2610/2610, SDMA6040/6040,617 flips,0 ACK timeouts/refusals, no TDR;
Windows and DWM retained. This is a warm PnP trial at the existing operating point,
not cold-boot, voltage-rise, full resume or complete startup acceptance.

M442 adds [RLC v2.0 header validation and PTE review](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-rlc-pte-review/RESULT.md).
M443 adds [hardware raster/completion observation and ordinary pending-vblank reporting](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-display-observation/RESULT.md).
Both are source/host work AFTER the frozen136 build, not deployed. Ambiguous
vblank observations remain deferred; local coherent PTE requests need lab observation.

M444 records the [unchanged136 raster baseline](../../evidence/windows/2026-09-24-E27-m9-recovery/raster136-baseline/RESULT.md):
1024 successful queries all report blank/line0. M445 adds
[runtime firmware metadata and explicit SMU policy](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-runtime-firmware/RESULT.md),
M446 adds [bounded POST recovery](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-post-display-recovery/RESULT.md),
and M447 records [memory review and PTE attempt instrumentation](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-memory-review/RESULT.md).
These changes are source/host validated, not deployed. The scoped GOP/PSP overlap
allegation is rejected; independent Windows mapping attributes remain unknown.
Bugcheck work does not implement complete GPU cancellation/reset. Runtime private
caps now require successful firmware load and native-owner startup; actual query
timing on unit A still needs acceptance.

M440's [typed client path](../../evidence/windows/2026-09-24-E27-m9-recovery/typed-smu-clients/RESULT.md)
and [actual client fixture controls](../../evidence/windows/2026-09-24-E27-m9-recovery/typed-smu-client-positive/RESULT.md)
precede the M441 handover. Their original source-only status is historical.

M438 completed Linux pre-driver thermal comparison and own AmdSetup extraction;
full SPI remains open. M439 deploys [candidate135](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07135-pitch-thermal/RESULT.md)
after returning to Windows: D3D,64MiB GPU readbacks, eight shaders and both E14
model outputs pass. GFX2610/2610, SDMA6011/6011,991 flips,0 ACK timeouts, no TDR.
Windows BAR/SMN thermal comparison passes24/24 bracketed samples. At M439 the
native SMU owner remained inactive; M441 completes the later client/startup handover. Physical geometry other than1920x1200 and12GiB remain untested.
Exact INF source/package bytes are retained in the [identity addendum](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07135-source-identity/README.md).
This supersedes the no-deployment statements in the historical source steps
below. It does not close the full startup/lifetime/cache/preemption gates.

M437 adds confirmed voltage-up staging before the AMD frequency-first clock
commit. Policy/transport/native-owner host controls and WDK build pass.
[Evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/clock-transition-order/RESULT.md).
At M437, client handover, temperature validation and startup integration remained
open; its20:53 read-only check retained M432134. M438-M439 supply later thermal
controls and deployment; native client/startup integration subsequently passes M441 PnP controls.

M436 implements primary pitch and inherited scanout geometry across allocation,
VidPn programming, mapping and CPU copy. 42375 host checks and full WDK build pass;
three deliberate regressions are detected. [Evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/scanout-pitch-geometry/RESULT.md).
No deployment or physical resolution/handover validation; lab134 remains M432.

M435 requires consistent VRAM capacity/base and a resolved POST reservation before
publishing the memory layout. Actual-source tests pass for8/12/16GiB configurations;
72checks, two detected mutations and full WDK build. [Evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/vram-geometry-consistency/RESULT.md).
No new deployment or12GiB hardware proof. The workspace DEFECTS.md owns review statuses; the [integration note](driver-review-2026-09-24.md)
links implemented work and evidence. Primary pitch hardware acceptance, scanout completion, restore
handoff and SMU upward-transition ordering remain part of the ongoing scope.

M434 adds a [native KMD SMU owner](../../evidence/windows/2026-09-24-E27-m9-recovery/native-smu-owner/RESULT.md):
23135 concurrent host checks, a detected stop/join mutation and full WDK build.
New bc250rd source removes direct SMU writes. Native temperature, client migration,
PnP/startup wiring and hardware handover remain unvalidated; no deployment.

M433 adds an [AMD-derived mailbox protocol layer](../../evidence/windows/2026-09-24-E27-m9-recovery/startup-smu-transport/RESULT.md)
and tests it with the real M429 clock policy: 607 checks and WDK object compile
pass; two protocol mutations are detected. No hardware access or deployment.
KMD ownership across bc250rd, native IO and startup wiring remain open.

M432 deploys [candidate134](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07134-dcn-publication/RESULT.md)
with the M430-M431 DCN/VidPn changes. One PnP update preserves OS/DWM; D3D pixels,
64MiB GPU readbacks, eight shader hashes and both E14 outputs pass. Final533 flips,
0 ACK timeouts/refusals, GFX2610/2610, SDMA6051/6051, no TDR. This supersedes the
source-only deployment status below, not the remaining M9 gates. The diagnostic
refusal hits the older full-WDDM dispatcher; its script expected the inner guard
and that failed observation is preserved. The [owner subsequently confirmed correct image and smooth movement](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07134-owner-feedback/REPORT.md).
Timed long-duration acceptance remains open. Native SMU ownership/startup work remains next.

M430-M431 add source-only DCN ACK/field-preservation and correct VidPn failure
publication. 31764 DCN and 61 publication checks plus full WDK build pass;
no new lab deployment. Hardware acceptance remains open. The
[BIOS follow-up](bios-analysis-followup.md) records D1-D7, the corrected Linux
manual-trigger path, and why a 12 GiB carve-out projects to only 11.5864 GiB of
application-segment capacity under today's reservations.

M429 adds source-only [startup clock preparation](../../evidence/windows/2026-09-24-E27-m9-recovery/startup-clock-policy/RESULT.md):
unchanged AMD commit function, serialized-owner contract, temperature gate and
readback;192host checks/WDK object compile pass. Native backend, cross-driver
ownership and startup wiring remain open. No new deployment; M428 runtime below
remains current. See [integration requirements](../design/startup-clock-ownership.md).

M428 validates a complete6GiB working set on unchanged133: six1GiB members stay
alive/resident together;103set/618individual queries report GPU-memory residency,
and all words pass GPU readback with6144fences. FinalGFX6784/6784,paging16614/16614,
noTDR,zero captures; OS/DWM unchanged through18:52:02.64MiB set and legacy controls
also pass. [M428 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/resident-set-6g/RESULT.md).
The owner's12GiB target remains open;6GiB is only37.5% of installed16GiB. A single
6GiB allocation failed creation; cause unknown. Budget/pagefile are not physical
VRAM. No startup or lifetime gate is closed by these content/residency samples.

M427 records a new OS boot18:23:50 for32GiB pagefile activation. Full-table
selection fails on one-shot registry persistence0xC000014D before GPU init.
One subsequent PnP transition restores full133;64MiB3-cycle/4-readback passes,
GFX256/256,paging1868/1868,noTDR, DWM2036/start18:24:54 retained through18:37:43.
[M427 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07133-pagefile-osboot/RESULT.md).
OS-boot/cold-power acceptance is still open; successful PnP does not close it.
The M426 workload results below describe the previous OS session.

M426 supersedes M423 with installed0.7.133.1,
SYS37A52F95CD90726D909FBF273D55B9336D766E2997668BA713B8ADC45BCF4A87.
All paging builders reserve OS-private slots; submission no longer allocates or
copies job metadata.907971 builder/711 queue checks,64MiB/1GiB GPU readbacks,
8shader/twoE14models pass.30673 queue admissions=SDMA submissions=completions;
GFX6706/6706,zero errors/noTDR. Boot/DWM retained through18:16:21, one PnP only.
[Candidate133 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07133-os-private-queue/RESULT.md).
Historical M423 baseline follows.

M423 supersedes the installed baseline below with0.7.132.1,
SYSE7AF5A02A3DEDA2CAD25E7D6A789FDA3C2406666D537F583D8ACAB14BDA49652.
Exact page disjointness fixes false DMA/data alias classification inside holes
of fragmented backing.64MiB,8shader/twoE14models and1GiB residency content pass.
The1GiB stage adds35native transfers and10GiB native data, with0capture plans;
this closes M422's observed large-transfer admission gap for these workloads.
Final native100transfers/90fills,82exact disjoint proofs,GFX6706/6706,
paging30672/30672,noTDR,sameOS/DWM through17:21:23. Restoration median1562ms
versus7719ms on131 is one ordered comparison, not Windows/Linux acceptance.
The context arena/physical fallback still exist; full resource/cache/lifetime,
cold/power startup and performance gates remain open.
[Candidate132 evidence and source](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07132-exact-dma-disjoint/RESULT.md).
Historical M422baseline follows.

M422 supersedes the installed baseline below with0.7.131.1,
SYS1F218318C57D4BC46D976B1AB4D43B659DB62233022FD1B9C0687EB1FEF9533B.
Native OS DMA copy/fill now executes through VMID2 with bounded generation,
typed metadata and CPU/GPU backing/permission checks; mapping probes8/8match.
Eight shader/twoE14model outputs and1GiB three-cycle/four-readback control pass.
Final native21transfers/89fills/5180551168bytes,GFX6450/6450,paging176923/176923,
zero errors/TDR,60reserved/0heap captures, same OS/DWM through17:02:35.
The1GiB test adds native fills but no native transfers; its large transfers
still use capture. This is acceptance of the exercised mixed path, not closure
of native admission, universal resource/alias/cache/lifetime or performance gates.
[Candidate131 evidence and source](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07131-native-os-paging/RESULT.md).
Historical M420/M421 baseline and development state follow.

M420 supersedes the KMD baseline below with0.7.130.1,
SYS BB96304B7A6F4C84819871CD5EAA3C240BFD3EF3BC9C9D0C9CD8F4F145F71D64.
OS paging DMA now comes from prepared aperture2 and carries nonzero GPU VAs.
Current8shader/twoE14model outputs pass; finalGFX2354/2354,SDMA22310/22310,
noTDR, same OS/DWM through16:22:43, M412/M414 modules/display gates retained.
Detailed CPU-PA probe lines wrapped; address identity is not proved.
[Candidate130 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07130-paging-dma-va/RESULT.md).
M421 adds [typed native private records and mixed ring consumption](../../evidence/windows/2026-09-24-E27-m9-recovery/paging-native-records/RESULT.md)
in source, with passing host/WDK checks. This DEV binary is not installed.
Builder generation and native OS execution remain pending; capture fallback
has not been removed. Historical129/128 baselines follow.

M419 supersedes the KMD baseline below with0.7.129.1,
SYS C4DC9A5870C0EB2D349CAB522077338B2DAC814C7A85661C71458D232AD36AE7.
Two VMID2 translated SDMA controls pass complete byte oracles, including a
same-VA VRAM-to-system remap. GPU-ordered root/TLB/IB packets are now hardware
witnessed for these owned pages. Current8shader and E14model regressions pass;
GFX2354/2354,SDMA22319/22319,noTDR, same OS boot/DWM through16:06:22.
Both SDMA test gates are0; M412/M414 modules and display gates remain.
[Candidate129 evidence and harness limits](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07129-sdma-va-complete/RESULT.md).
Ordinary OS paging still uses captured physical commands. Next integrate typed
OS DMA IB ranges and a system-context CSA policy; arbitrary ownership, queued
GPU PTE updates and preemption are not accepted by this owned-page control.

Historical128 baseline follows:

Unit A retains full WDDM0.7.128.1 in boot11:44:14, stage50,
UnconfirmedStarts0. SYS SHA256 is
98931C11FAF37DE35A230022839C5B68C69A6A1999CD39438987F2BD20DA4897.
M417 performs one PnP transition with pre-publication direct/indirect SDMA
4KiB/64KiB controls, all bytes/fences correct. Post-start8shader and both
E14 model references pass; GFX2354/2354,paging22318/22318,noTDR.
Boot11:44:14 and DWM4448/start13:58:18 remain; EnableSdmaIbControl is now0.
[Candidate128 evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07128-sdma-ib-complete/RESULT.md).
M404 first warm start passed shader/model and1GiB readback controls; M405 added
a second warm start with64MiB readback. Subsequent M406-M412 display experiments
also restarted the adapter in the same boot without OS/AC recovery.

Preserve the [M13.1 visible desktop](m13-present-baseline.md) while completing M9.
M412 leaves Mesa26.3.0-devel/LLVM23.1.2 llvmpipe CPU JIT registered and PresentBlit/DcnWrite/VidPnFlip enabled;
visible desktop, corrected buffer rotation and fluid mouse motion with the
overlay were confirmed by the owner on M410. M412 repeats the image/content
controls; current-build input feedback and30minute acceptance remain open. GPU compute gates
remain enabled; the final desktop-session64KiB control passes four complete GPU
readbacks and three residency cycles with graphics4/4,paging829/829,noTDR.
On-disk Full0 is consumed one-shot state, not display-only. Preserve the working
UMD/display configuration in future compute scripts; historical scripts close
these gates and register the stub. M9 acceptance remains open.

See [M404](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07127/RESULT.md),
[M405](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07127-repeat/RESULT.md)
and [M412](../../evidence/windows/2026-09-24-E26-desktop-resume/mesa-main/RESULT.md).
Successful repetitions do not establish partial-failure lifecycle reliability
or the internal firmware cause of previous hangs.

M414-M415 migrate the Vulkan compute path to Mesa main f333dd6d, ACO,
with the BC250 WDDM2 integration. Use the
[current launcher and build recipe](../../experiments/E27-m9-inference/radv-main/README.md)
for new compute work; old ICDs remain isolated comparison artifacts. All8shader
hashes and both E14 model references pass. Sparse mappings remain unsupported
where the required PRT alias policy is absent. Current paired TinyLlama
1115.94/114.56 versus1078.17/111.38 tokens/s shows a session-local improvement,
not a general speedup or Windows-over-Linux advantage. Same127/M412 desktop;
finalGFX33471/33471,SDMA88076/88076,noTDR or hardware/OS resets.
See [migration evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/radv-main/RESULT.md).
M413 is retained as the earlier current-desktop benchmark with a task observer
anomaly. M376's earlier SSH loss remains unexplained. M377/M378 controls remain
historical evidence on119. Windows Terminal crashed during a launch in M414;
explicit headless conhost avoids it for compute but does not fix desktop compatibility.
Capture arena fallback/resource guarantees and cache/PFN/lifetime requirements
remain open. The table below distinguishes workload evidence from these contracts.

## Requirement-by-requirement status

| Requirement | Evidence that exists | What still prevents acceptance |
|---|---|---|
| Shared initialization, prepared firmware, explicit phase ownership and unwind (startup1-2) | M233-M240 source/host failure and interrupt controls; M241 automatic admission integration | M404 first/warm127 and subsequent M419-M423 PnP transitions with post-start content pass. Device remove/recreation after failed hardware retirement remains unverified. |
| Engines and queues ready before first supported OS work; no production CLI RUN dependency (startup3-4) | M241 integration; M250/M251 actual early paging/queue delivery; M271 cold-session07100 completion | M423132 starts engines before OS paging and passes shader/eviction/inference without errors/TDR. Cold132 and partial-failure lifecycle acceptance remain open. |
| Cold start, repeated start after workload, partial failure, map/transfer/unmap ownership (startup5) | M271 cold-session CP1..8 and paging; earlier retention tests | M404/M405 and M419-M423 provide repeated PnP starts with post-start GPU content. Current133 cold/power transitions, partial failures and complete mapping/PFN ownership remain unverified. |
| Physical/system/fragmented bytes, private/DMA ranges, multipass and table ordering | Host integration through M311; earlier M257 real1GiB residency cycles and M283 shader visibility on07100 | M423 adds exact fragmented DMA admission; native ordinary work, host wide multipass/permissions and real1GiB readback pass. Individual captured dependency shapes, PFN lifetime and full cache aliases remain unproved. |
| Aliases | Equal-offset graph cycles; unequal-offset compatible-direction dependencies including repeated source reads; actual self-table host walks | Conflicting-direction unequal intervals and repeated destinations remain unsupported. MS descriptions inspected in M303 do not define arbitrary overlap snapshot semantics; do not present robustness coverage as observed OS demand. |
| Restricted BuildPagingBuffer outcomes and resource readiness | Capacity exhaustion separated from builder errors; exact publication ordering tested | M323/M324 reserve nonpaged storage before SystemContext publication; M336 witnesses20reserved plans on105; M375/M376119 shares available reservation spans; measured sequential capture plans use it, while simultaneous spans remain a host witness; exhausted/oversized fallback still allocates paged pool. Allocation failure and unsupported dependencies propagate internally through the actual outer return path. M423 admits the tested large residency transfers natively with0capture plans; the allocating physical fallback remains. A guarantee for all supported work is missing; never replace these errors with empty success or an infinite insufficient-buffer retry. |
| CPU aperture/cache policy | Separate table storage and prior mapping work; M287 physical-range cache query instrumentation | M378119 shader positive/eviction/stale-control paths pass; the older physical cache query failed with0xC0000141, so exact cache-attribute acceptance is still missing. Physical-range cache type alone does not prove actual Windows mapping aliases. All overlapping views and actual coherency require acceptance. |
| Context/capture/queue lifetime and honest completion | Captures owned by context, accepted progress, completion detach, stop cleanup; host submission/retirement tests | M377119 adds two concurrently launched64MiB clients, both byte-oracle passes with12reserved/0heap and bounded post-completion observations. Same-system-context callback interleaving, actual OS cancellation, PFN ownership and device-generation transitions remain unverified. No timeout-based claim of hardware halt is allowed. |
| Paging pressure and performance | M257 full1GiB GPU readback and M283 shader test on older builds; M274 Windows TinyLlama below Linux | M423132 full1GiB native path passes with0capture plans; same-session restoration median1562ms versus7719ms on131. This is not inference or Linux comparison acceptance. M321 remains an older inference baseline. M338105 TinyLlama1101.13/118.28 remains below historical Linux1119.59/154.92, with configuration caveats. Broader pressure/concurrency and Windows superiority are not achieved. |

## Next actions, in order

1. Preserve M412desktop/M414RADV and the M426133 working session. The native
   path passes shader/model and64MiB/1GiB content controls. Current133 cold/power
   transitions and partial-failure startup remain open; no routine reset.
2. Resolve remaining paging resource/status and dependency requirements.
   M423 closes the measured false-alias gap: large ordinary residency transfers
   now use native DMA and the entire tested workload creates0capture plans.
   M426 removes per-submission job allocation/copy and passes live controls.
   OS retention, preemption/cancellation and generation acceptance still need
   explicit validation; do not infer them solely from completed workloads. Preallocated context arenas
   and physical/table/alias fallback remain. Prove
   bounds for supported fallback work and remove hot-path pool dependence while
   preserving table publication, simultaneous ownership and cancellation.
   A cap or infinite insufficient-buffer retry is not a resource guarantee.
   Forced CSA preemption/restoration and companion roots remain separate gates.
3. Establish exact physical ownership and compatible cache attributes for
   retained/borrowed views. Shader and residency passes do not identify every
   Windows CPU mapping alias or guarantee PFN lifetime.
4. Validate partial-failure retirement/device-generation transitions after the
   positive repeated-start path. Retain resources if hardware retirement is
   unconfirmed; honor STOP, clocks, temperature and overlay.
5. Measure equivalent Windows/Linux inference on the accepted implementation,
   profile costs and optimize. Historical performance below Linux remains the
   baseline until replaced by comparable measurements.

Host-only work can continue on source requirements if lab access is unavailable. No completion claim follows from this index. See [M312 source review](../../evidence/windows/2026-09-23-E27-m9-inference/acceptance-review/source-state.json) for the earlier allocation/status audit; deployed source snapshots are in M317 evidence.


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
M329 adds07104shader/content acceptance; captured-transfer use witness still missing. Prior07103warm failure
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



### M330 recovery blocked after third consecutive observation

Initial warm trial and two follow-ups have no reachable lab SSH endpoint; the
initial full-subnet discovery also found no pinned lab. Original session55183
remains live with no new output, not terminal and not eligible for a duplicate
trial. Monitor response is pending. M331 source/reference review is complete;
next diagnosis requires recovered stop/start snapshots. Goal blocked pending
owner observation or restored connectivity, not complete. Prepared read-only
warm07104-recover.ps1 is parser-checked. No reset or further GPU action performed.
Evidence: warm07104/observation-3.json. On resume recheck original handle and
endpoint identity, then acquire persistent logs before any initialization.


### M332 - Connectivity restored; warm failure boundary recovered

M330 recovery connectivity blocker is cleared. Read-only acquisition after AC restoration found the failed warm-start final snapshot at CP1 scheduler-read, before the next scheduler-write checkpoint. Current boot03:06:12 is healthy display-only with the exact104 SYS. Stop evidence reaches79. GuardLogKeep completion versus MMIO is not distinguished; RLC retirement remains unproven. See facts M332 and warm07104-recovered/RESULT.md. No new initialization or driver deployment.


### M334 - Linux reentry reference acquired

E28 records successful first load and SMU-layer RLC retirement during successful unload, then a failed second load after RLC enable. This closes the missing runtime stop-path witness and supplies partial reload ordering, but does not supply a working Linux reset/reload sequence. The7-versus8 RLC write difference is consistent with retained SPM VMID15. Keep warm Windows acceptance open; see M334.


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


### M338 - Inference measured on105

TinyLlama1101.13prompt/118.28generation tokens/s; no causal improvement or
Windows superiority claim. UMD function10365calls/159.77ms excludes whole
submission/completion latency. No restart/errors/TDR. See facts M338.


### M339 - Bounded concurrent-client acceptance

Two concurrently launched64MiB clients pass three cycles/fourfullGPUreads each,
reserved75->87,heap0,noerrors/TDR. No same-context interleaving or general resource
bound inferred. Local VirtualTransfer docs do not close that guarantee. See M339.


### M340 - Retirement observer prepared

Same104 first/warm PSP reports match all11commands except timing, allrc/status0.
No reported PSP rejection explains the warm hang. E28 lacks a complete post-stop
GRBM_STATUS2 witness. New read-only RLC retirement snapshot builds locally; no
reset or quiet-predicate change, not deployed. Next capture the new observation
before choosing a recovery sequence. See M340.


### M342 - RLC positive control and teardown boundary

First106start/64KiBGPUprobe pass. Busy clear before/afterPSP and immediately
afterGFXstop, but set at later read after complete teardown. Current106display-only
has onlyMMIOread gate enabled,allGPU/writegatesclosed,sameboot04:16:46.
Next isolatePSPunload/GARTboundary; no causal reset-fix claim. See M342.

### M380 - Current inference performance

See [benchmark119](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07119-benchmark/RESULT.md). Current119 inference performance remains close to105 within measured dispersion; no optimization claim. Final independent CLI succeeds; initialized session retained. Full acceptance gates remain open.

### M381 - Capture demand source review

[Source review](../../evidence/windows/2026-09-24-E27-m9-recovery/capture-demand-review/RESULT.md) establishes construction completion as the arena release point, before GPU fence completion. Size resources for simultaneous unfinished construction, not queued GPU buffers. A numeric bound and allocation-free accepted path remain open. No hardware change.

### M382 - Construction pressure witnesses, local120

[Instrumentation](../../evidence/windows/2026-09-24-E27-m9-recovery/capture-pressure/RESULT.md) records active plans/reserved spans and independent per-context maxima retained by adapter.329199host checks and WDK/package pass. Not deployed;119 initialized session retained. Hardware demand measurements and guaranteed resource readiness remain open.

### M383 - Shader visibility through buffer aliases

[Hardware test](../../evidence/windows/2026-09-24-E27-m9-recovery/shader-buffer-alias119/RESULT.md) passes16baseline and16same-memory alias rounds; disjoint-memory negative control fails as expected. FinalGFX13298/13298,paging183579/183579,noerrors/TDR,114reserved/0heap. Two VkBuffer objects at one binding offset do not prove distinct GPU VA or Windows CPU cache attributes. Same119 full session retained;120 not deployed.

### M384 - Produced alias data across eviction

[Alias eviction](../../evidence/windows/2026-09-24-E27-m9-recovery/shader-alias-eviction119/RESULT.md) passes16rounds with3cycles after producer and before reader; baseline matches and stale control discriminates. FinalGFX13340/13340,paging209594/209594,noerrors/TDR,134reserved/0heap. Same119fullsession retained;120 undeployed. This is same-allocation Vulkan alias coverage, not general paging alias or cache-attribute acceptance.

### M385 - Runtime-version Linux source comparison

[Source review](../../evidence/windows/2026-09-24-E27-m9-recovery/reentry-source-61852/RESULT.md) pins upstream6.18.52 and matching Alpine recipe. Selected reset/firmware functions largely unchanged; queue-reset advertisement and retirement ungating order differ. Next reference measurement must witness actual reset dispatch. No working warm sequence or new driver change established.119 session retained;120 undeployed.

### M386 - Per-queue reset probe preparation

[Probe](../../evidence/windows/2026-09-24-E27-m9-recovery/sdma-reset-probe-preparation/RESULT.md) separates timeout-driven reset from manual debugfs full reset. Source-derived packet generation and10construction fixtures pass; no hardware execution. Next finish tracing/runtime preflight, then positive control before any delayed job.119 fullsession retained.

### M387 - Exact reset tracing prepared

[Scripts](../../evidence/windows/2026-09-24-E27-m9-recovery/sdma-reset-trace-preparation/RESULT.md) provide isolated20probe entry/return trace and first-load timeout preflight; local syntax/source resolution pass. Kernel attachment, positive control, timeout dispatch and fresh-context post-reset acceptance remain unexecuted.119session retained; no lab action.

### M388 - Working Linux queue-reset reference

[E29](../../evidence/linux/2026-09-24-E29-sdma-reset/RESULT.md) records one timeout-selected SDMA reset with fresh SDMA marker/fence and compute256/256-word controls afterwards. Original job times out; full-GPU reset callbacks not entered. Warm firmware reload and Windows recovery remain open. Linux currently running, USB loader on;119 installed,120 undeployed. Next compare source-matched MMIO ordering and retained backing against Windows retirement.

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
