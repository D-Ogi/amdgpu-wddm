# E27: M9 Vulkan inference

Current compute baseline (M414-M415, 2026-09-24):
[current Mesa RADV/WDDM2 source, launcher and measured results](radv-main/README.md).
Preserve the M412 working display gates and CPU llvmpipe desktop during compute
work. Earlier procedures below are historical trials, including their old ICDs
and display-only restoration steps; do not replay them as the current startup policy.

Hypothesis: the M8-validated RADV WDDM path can execute llama.cpp9564 inference with the same greedy output as the Linux E14 reference, first with stories260K/stories15M and then TinyLlama1.1B Q4_0.

PROVENANCE: llama.cpp, ggml-org/llama.cpp b9564 (3b3da01dc21dc68e958efb898ab739c65ed08ca2), MIT; official Windows Vulkan release. Models are the public E14 downloads, verified against llama-install.txt SHA256 values.

Procedure: verify downloads and version, CPU positive control on the lab while display-only stays active, then separately revalidate M8 on installed KMD before inference on the GPU. Begin with one16-token stories260K dispatch and a bounded deadline. On success run E14 prompts/options: stories15M96 tokens, TinyLlama64 tokens, temperature0, seed1, ngl99, no conversation mode; four fresh processes each. Preserve raw stdout/stderr and hashes. Compare raw bytes first; also report CRLF-to-LF canonical text separately to distinguish platform line endings without concealing token/content differences. CPU is the control for the two small models; Linux GPU output is the final reference, particularly for TinyLlama.

Performance: pp512/tg128, stated1000MHz clock, compare M52; investigate differences before closing M9. Allocation/residency/paging and completed hardware submissions must be evidenced. Stop above85C. No GPU test starts until M8 regression and thermal controls pass. Each full-WDDM test has bounded execution and display-only restoration. Never treat device listing or CPU fallback as a GPU pass.

Expected: matching model hashes/version and CPU control validate the harness; GPU hash mismatch, timeout or allocation fault identifies the next driver/compiler investigation. This experiment does not close the full WDDM desktop goal.

First GPU probe after M151: tiny-gpu.ps1, stories260K16 tokens, ngl99. Same gate/bring-up/fence controls as M8 regression;90s inference deadline and2s temperature samples. Require Vulkan device/offload evidence and hardware submission progress; compare generated text to the same CPU16-token control. This is a smoke test, not the four-run M9 acceptance.

Run002 follows a physical reset (boot01:57:26) and verified display-only recovery. Unlike run001, this is the first engine bring-up of the boot. Same tiny model and driver; stream gate/engine/inference boundaries through target_stream.py to a local exclusive log. This isolates repeat-start state as a candidate, without assuming it caused run001. Output directory is new and cannot be overwritten.

Run003: after a normal Windows reboot, test-backend-ops from b9564 with unchanged release Vulkan plugin. CPU CPY f32 control64/64 passes. GPU selectorVulkan0, CPY f32 plus small F32 RMS_NORM/SOFT_MAX and32x32x32 MUL_MAT. Require actual named Vulkan backend and nonzero executed tests; skipped backends alone cannot count as success.120s bound, temperature polling and display-only restore. Reboot separates this from the known second-entry problem; it does not fix that lifecycle defect.

Run004: corrected matrix/softmax/copy selectors match27 CPU cases, all pass. In one bounded120s GPU session after reboot, run those27 cases then three independent stories260K16-token invocations: unchanged baseline, GGML_VK_DISABLE_F16=1, and GGML_VK_DISABLE_GRAPH_OPTIMIZE=1 plus GGML_VK_DISABLE_FUSION=1. Per-variant raw output/exit witnesses retained. These are diagnostic contrasts, not accepted fixes; a changed output requires further isolation and regression.

Run004 result: see M155; all27 operators pass, allGPU inference contrasts remain incorrect. Run005 hypothesis: async scheduling or direct host-visible video-memory access contributes to the divergence. After a fresh boot, compare baseline, GGML_VK_DISABLE_ASYNC=1, GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM=1, and both, keeping all prior optimization flags cleared. Same CPU16 control and120s bound. A successful contrast identifies a path to inspect, not a final driver fix.

Run005 also runs the upstream b9564 llama-eval-callback against CPU then GPU with diagnostic flags cleared. Node samples/sums can identify a first divergence; callback reads impose synchronization, so a passing callback is not proof the normal graph works. The host-visible flag may leave allocation selection unchanged for UMA and is not evidence of staging without confirming the chosen memory type.

Correction from recovered stop logs: run005 has67 hardware submissions,66 completions,1 timeout and1439 jobs not run. Later contrasts cannot isolate math or synchronization. Run006 runs the diagnostic node-probe FIRST on a fresh boot, upstream callback modified to dump F32 sources and destinations, and save KMD summary after each node; abort callback as soon as a hardware timeout/refusal is observed. CPU control first does not initialize Vulkan (ngl0 still loads backend; no GPU graph). This separates the first real failure from subsequent software completions. Node bytes, raw log and failure exit retained. Bound120s and restorefinally unchanged.

Run006 incomplete: per-node log escape required admin, diagnostic callback returnedfalse without decode failure. First RMS_NORM input and output nevertheless match CPU (M157). Run007 repeats on a fresh boot with elevated interactive task solely for log collection and an explicit callback-failure exit witness. Per-node logs/tensors cannot silently skip on observer failure.

Run007: all161nodecallbacks complete,536hardwaretasks executed, final top tokenCPU/GPU432 (M158). Run008 isolates --no-warmup on the official completion binary, freshboot, ordinary batching with no callbacks or flag overrides,16tokens CPU/GPU. Require matched text AND final zero hardwaretimeouts/refusals/UMDnotrun. No warmup is diagnostic, not a final repair.

Run008 no-warmup stilltimesout(M159). Run009 hypothesis: the packed IB can be overwritten when the previous submission lacks an application signal. mesa-gather-fence.patch (Mesa MIT, incremental over driver/icd patch) gives each queue its own gatherBO and private monitored fence, signaled after every hardware submit and waited before reuse. Test isolated ICDpath, M8eightworkloads x3 then ordinary official16-token inference with normalwarmup, all diagnosticflags cleared. Require correct text, allhardware completions andzero timeouts/refused/notrun. Progress trace records previous/observed ownfence and applicationwait/signalcounts. This is an unvalidated fix candidate, not a proven cause.

Run009 invalid for candidate: elevated app ignoresICDoverride; no newprogress traces (M160). Run010 repeats withLimitedtask andVK_LOADER_DEBUG=driver, requirescandidateprogresslineinM8stderr beforeCPU/GPUinference. ElevatedouterwrappercollectsKMDcounters. No per-nodeescapeinsideapprequiresadminhere.

Run010 acceptance additionally requires the candidate progress trace in inference stderr, a verbose positive full-layer GPU-offload witness, and equal positive submitted/completed hardware counts. These prevent treating CPU fallback or pending work as success. This remains a tiny-model diagnostic, not M9 acceptance.

Run010 aborted before engine initialization: null DWM StartTime in E19 observation. Restored display-only with runtime gates closed. Run011 repeats the same test after making the process-time observation null-safe. No GPU-bearing session has occurred this boot; no additional reboot required for this retry.

Run011 passes ordinary tiny inference and M8 (M161). Run012 expands the same candidate to four fresh processes each of stories15M Q4_0 (96tokens) and TinyLlama1.1B Q4_0 (64tokens), matching E14 Linux GPU reference text after CRLF normalization only. Require actual candidate trace, full GPU offload, positive equal hardware submissions/completions and no timeout/refusal/notrun. M8 control first,300s bound, temperature2s, restorefinally. Fresh reboot before this GPU-bearing session. Performance/paging remain separate acceptance work.

Run012: four stories15M and two TinyLlama match Linux; third TinyLlama triggers scheduler preemption then TDR, M162. KMD0.7.57 candidate defers DMA-boundary preemption while hardware or submit DDI is active and until completions are published; completion state changes under one lock, per-node reported fence, serialized report DPC. Run013 repeats012 on fresh boot with this KMD and unchanged candidate ICD. Explicit nonzero CMD exit check catches negative NTSTATUS; live watch rejects TDR as well as ring failure. Require actual preemption request/report witness before claiming the preemption path is validated. Primary contract: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_preemptcommand .

Local DDI documentation source supplied by owner: ref/windows-driver-docs-ddi, commit7515063cea4c9e98db6a92986c5b4ddb0463fd16, wdk-ddi-src/content/d3dkmddi/nc-d3dkmddi-dxgkddi_preemptcommand.md and ns-d3dkmddi-_dxgkargcb_notify_interrupt_data.md. Use this checkout for subsequent DDI research.

Run013 passes eight reference-matching processes and29live preemptions, M163. Run014 measures official b9564 llama-bench pp512/tg128,3repetitions each for stories15M and TinyLlama, at explicitly requested1000MHz820mV. Same candidate DLL/KMD; verbose diagnostic logging remains enabled, so treat these as instrumented baseline performance. JSON raw data, hardware/TDR/temperature controls, M8 first. Compare with M52 and explain any gap before closure. Does not claim eviction merely from model allocation.

Run015 tests reduced UMD diagnostics: optional BC250_TRACE_SUBMITS=1 restores per-submit/IB tracing; quiet mode retains first queue progress line and errors, and vk_async_event stops printing successful waits. Incremental mesa-quiet-submit.patch after the consolidated private-fence ICD. Same KMD0757. M8, four fresh processes per reference model, then pp512/tg128 r3 for both. Clock1000/820. No OS reboot: first controlled repeated full-mode entry in this boot per owner preference. Stream gate/engine stages and restorefinally; if entry stalls, inspect existing state rather than start another job. This isolates UMD diagnostic overhead, not KMD tracing, and does not validate paging.

Prepared residency-probe.c reuses bounded kmtprobe helpers and the canonical BC2A contract. CPU pattern control, explicit Evict, QueryAllocationResidency NOTRESIDENT witness, equally large dirty pressure allocation, MakeResident, full readback over3cycles; VRAM/GTT up to1GiB,120s watchdog. Build and --help smoke pass; NOT RUN on lab. Begin64KiB before1GiB. Requires KMD paging counters and separate GPU consumer before claiming GPU paging correctness.

Run015 interrupted atCPstage6 on repeated initialization, M165; no quiet ICD evaluation occurred. Run016 on recovered boot04:20:42 combines quiet ICD correctness/benchmark with residency controls in one GPU session. VRAM/GTT64KiB first,1GiB only if respective small control passes; report residency outcomes separately from GPU text/benchmark acceptance. No OS reboot issued. Recovery closed runtime gates before experiment.

Run016 results are recorded in M166. Offline validate-016.py recovers text/hardware/benchmark validation after the wrapper positional Select-String error; fixed explicit -LiteralPath/-Pattern. Residency probe now accepts shared-memory status2 after Evict and polls up to5s for departure from GPU-memory residency. This is a CPU lifecycle control, not a weaker substitute for actual GPU paging acceptance. Revised probe builds /W4 /WX and passes --help; new lab execution remains pending.

M168 local0759 preparation: portable page-wise packet builder integrated into GfxPagingBuild with byte-based MultipassOffset, partial publication and live-ring budget. driver/kmd/test/run_paging_stream.ps1 validates fragmented copy/fill bytes and synthetic1GiB exactly-once command coverage; real AMD packet22checks also pass. Candidate not deployed: system-page mapping, per-buffer lifetime and fault completion must be completed before lab acceptance.

M169 local0760: per-OS-DMA private records replace shared command shadow; exact-range parser and independent pending-buffer tests pass. M170 local0761: actual watchdog functions no longer synthesize completion on timeout; old fails/new passes both-node/late-fence/on-time controls. Neither package was deployed. Remaining audit items include system mapping, malformed virtual submission/error retirement, refusal handling, actual TDR recovery and engine-state rundown.

Local virtual-rejection experiment: extract the actual report DPC, exercise rejected fences behind busy GFX/SDMA, active DDI and a completion arriving during publication. Require only the real predecessor to produce DMA_COMPLETED, then preemption to report the rejected fence. Controls include first high-bit ID, wrap, later completion, and real late completion after watchdog. Build the full KMD; this host test does not establish OS error-device behavior.

M171 local0762 results: validation old FAIL/new PASS, expanded actual report-DPC tests PASS, watchdog/parser regressions PASS, signed candidate built. Evidence virtual-rejection-0762. Not deployed; valid ring refusal, non-UMD malformed packets, system mapping, rundown and hardware reset recovery remain open.

Local0763 lifetime test: actual GfxPagingBuild and GfxStop entry/exit extracted into a Windows host harness, kernel push locks modeled by SRW locks. Two builders block inside the packet builder; stop must wait beyond the former200ms timeout. Disabling builder locking must fail the lifetime assertion. Verify post-stop and early-exit unlocks; run stream/private packet regressions and full kernel build. This does not validate interrupt rundown or GPU reset.

Local0764 CPU access lifetime: extract actual admission/release/close/open functions and GFX/SDMA fence callbacks. Windows SRW/event model holds two reads while close must refuse new admission and wait for both. Drain-disabled mutation must fail. Test event reuse/reopen/null/early return, existing builder lifetime and paging-private regressions, then full KMD build. CPU lifetime references do not prove GPU DMA has stopped; WDDM object/interrupt teardown needs a separate audit.

Local WDDM stop ordering test: ISR entry only accesses IH/DCN device state; WDDM reads happen in the DPC. Extract actual stop cancellation/detach/flush sequence and inject a new pointer capture at the end of the flush boundary. Old ordering must leave a stale reader, new detach-before-flush must refuse it while joining prior readers. Check armed/disarmed vsync controls and full kernel build. Host ordering model only, not proof of every driver lifetime contract.

M178 local0766: virtual system-page mapping integration, shared64MiB GTT boundary plus two-page window, scratchslot5 and command-offset phase markers. Extracted actual resolver/emitter linked with real packet/page-stream helpers passes364checks; window/lifetime controls and full build pass. Evidence system-paging-0766. No hardware test or deployment yet; physical ADL, OS page lifetime and error/refusal paths still require review.

M179 local0767: valid dispatch refusal blocks software retirement/preemption, late real predecessor remains valid, and ResetFromTimeout returns failure without dropping pending fences because actual GPU halt is unproven. Extracted refusal/watchdog/reset/report tests and full build pass. Failed recovery can bugcheck/restart Windows; no deployment or runtime test performed. Evidence refusal-0767. Unsupported BuildPagingBuffer paths and successful reset still need implementation.

M180 local0768 preparation: CP begin/result breadcrumbs narrow stage6 for the next controlled hardware initialization. Existing replay matches354+35 reference writes and its recovery-model controls pass; signed build passes. No deployment or hardware experiment this turn. Evidence cp-diagnostics-0768; collect guard logs live and retain final available substep, accounting for buffered log loss. Successful hardware recovery remains unimplemented.

Local VMID flush packet experiment: generalize existing SDMA GFXHUB invalidation to VMID0..15 while retaining VMID0 GART wrapper. Require request encoder and ACK mask to select the same VMID, reject16/UINT_MAX before shifts/output, preserve undersized buffers, and retain all mapped-transfer routing controls. Run run_paging.ps1 -KmdRouting and full KMD build. This groundwork does not implement the FLUSH_TLB DDI: process/root binding, cross-engine ordering and page-table update publication remain separate requirements. No lab run planned in this step.

Local0769 flush integration: route FLUSH_TLB through per-buffer records to SDMA, invalidate full VMID1 (all WDDM jobs currently use1), keep outer completion after ACK. Extract actual GfxPagingBuildFlush for host tests: ready/not-ready, app-vs-GART VMID, all command offsets across ring boundary including outer fence, no partial publication, balanced lifetime lock. Full build and private-record regression required. Bootstrap/not-ready and generic builder error policy remain OPEN; no hardware deployment in this step.

M182 local0769 result: extracted ready/not-ready flush builder and packet/budget controls pass2560checks; private-record regression and full build/sign pass. No deployment. Evidence flush-integration-0769 explicitly retains bootstrap/error fallthrough and GPU_PHYSICAL PTE ordering as open requirements.

Ordered PTE packet preparation: compose explicit AMD WRITE_LINEAR with the existing noninterrupting fence/memory-poll barrier, before any later FLUSH_TLB. Test counts1..512, exact aligned capacity and one DWORD short, per-entry values, barrier address/marker, untouched tails and invalid input. Run existing KMD routing suite and full build. This helper is not yet the UPDATE_PAGE_TABLE DDI; CPU_VIRTUAL bootstrap must remain immediate and GPU_PHYSICAL integration needs multipass and explicit engine-state handling.

M183 result: ordered-PTE constructor and existing routing suite pass6182checks; full dev KMD build/sign passes. No runtime integration/deployment.512entries exceed live ring reservation; source-derived maximum497per otherwise-empty buffer, with15remaining. Evidence ordered-pte-packets records bootstrap/multipass/publication requirements.

Local0770 PTE integration test: extract actual VidMm encoder/root resolver and GfxPagingBuildUpdate; field-level WDK model with real AMD translator/packet constructors. Check480+32 entry multipass, MC conversion, Repeat/StartIndex, marker offsets, complete source validation, no output when full, bootstrap CPU dispatch and no fallback after bootstrap close. CPU write routine is stubbed; full KMD build checks real WDK types and fixed stack budget. No hardware deployment planned.

M184 local0770:6197host checks, PTE translator/private-record regression and full build/sign pass. Fixed stack frame3992bytes stays under4096checker limit (warning retained);512PTE split480+32. Candidate not deployed. Evidence pte-integration-0770 explicitly lists CPU update mapping errors and outer builder failure policy as remaining gaps.

Local0771 CPU PTE failure test: replace host CPU-write stub with extracted actual VidMmUpdatePageTable, real AMD translator and modeled pool/MMIO mapping. Inject snapshot allocation and physical mapping failures; require no published progress, no writes/leaks. Invalid last PTE must leave entire table unchanged before mapping; CPU_VIRTUAL Repeat must use supplied aligned pointer without mapping/unmapping. Full WDK build and prior paging controls required; modeled pointers valid, no SEH exception injection.

M185 local0771:6210host checks and full build/sign pass. Actual CPU updater tested with map/allocation failure injection; destination unchanged on invalid final entry and bootstrap progress stays zero on failure. Evidence cpu-pte-errors-0771. Not deployed; OS-facing BuildPagingBuffer failure handling remains incomplete.

Local0772 removes pool allocation from CPU PTE update by embedding512entries in VidMm state, serialized with an exclusive push lock. Wrapper lock order follows GfxPagingLock; VidMmStop joins CPU updates. Existing extracted CPU/mapping failure tests must pass, mapping hook asserts lock ownership, all exits balance locks. This harness models lock ownership, not actual contention; no lab deployment.

M186 local0772 results: embedded CPU snapshot removes allocation failure point;6213host checks/full build pass. Extracted wrapper/stop SRW concurrency test: unlocked mutationFAIL4, realPASS0. Existing builder lifetime regressionPASS. Evidence cpu-pte-storage-0772. No deployment; map failures and outer OS error contract remain unresolved.

Local0773 retained VidMm map: map bounded segment once at start; CPU physical updates and shared-lock page walks reuse it; stop drains mapping users then unmaps. Start mapping/pool failure propagates through WddmStart to StartDevice cleanup. Extract actual start/stop/walk for host bounds/leaf/lifecycle tests, extend CPU lock test for deferred unmap. Validate full WDK build; no hardware deployment. Mapping resource cost and full StartDevice cleanup need runtime validation.

M187 local0773 results:6221host checks, updated ownership mutation/control and full build/sign pass. Retained mapping replaces per-call maps for CPU updates/normal page walks; mapping failure now occurs at StartDevice. Evidence vidmm-map-0773; no hardware deployment. Full PnP cleanup and resource/cache costs require lab validation.


### 0.7.73 startup-only acceptance

Hypothesis: candidate 0.7.73.1 installs and starts in display-only mode with all
GPU execution gates closed, without an OS restart. Run `install-0773.ps1` through
`tools/win/target.py` after checking the overlay STOP flag. The script checks the
package hash, temperature, initial gates, installation status, active PnP version,
installed image hash, unchanged boot, DWM presence and driver escape before
confirming the start. A restart request or failed check stops the procedure.
No engine RUN is issued. This does not validate the retained VidMm mapping,
which requires full WDDM and GPU VA gates, or any new GPU paging path.
Result: startup acceptance passed; see facts M188 and `startup-0773` evidence.
The installed lab package is now0.7.73.1 (oem67.inf), with GPU gates closed.


Reader-lifetime validation now also holds two shared walks, excludes a CPU writer,
and joins a held reader before unmapping at stop. Both lock-removal controls fail;
actual wrappers pass (M189). Full WDDM/GPU-VA activation remains deferred for the
cache-alias policy review documented in M190; display-only candidate0773 remains
installed with gates closed.


### Queued PTE ordering acceptance probe

After `run_paging.ps1 -KmdRouting`, run the resulting `paging_packets.exe` with
`--queued-pte-ordering`. It isolates a queued leaf update followed by actual CPU
translation before execution. Expected correct construction resolves the new
page; current source resolves the old page and exits1 (M191). Default6221 checks
still pass and do not include this failing acceptance requirement. Actual OS
reachability of this sequence is unverified; immediate CPU_VIRTUAL initialization
is a passing positive control. No hardware test was run.


M192 adds logical PTE construction-state groundwork (`run_paging_pt_shadow.ps1`):
544 host checks and dev KMD build pass. Microsoft documents the M191 ordering
dependency. Helper is not runtime-wired; queued-PTE acceptance still fails.
See `paging-pt-shadow` evidence for integration gates and source links.


Candidate0774 integrates logical-state reservation, immediate CPU initialization
and teardown (M193).6229 route checks and557 core checks pass; concurrency controls
fail/pass as expected. Queued-PTE ordering probe still fails1 of8 checks. Full
build/sign passes; not deployed. Evidence: `shadow-start-0774`.


Candidate0775 wires accepted logical PTE publication and paging-only logical
translation (M194). The previously failing M191 scenario now passes539 checks
and is included in the default6768-check routing suite. `-OmitLogicalCommit`
requires `-KmdRouting` and deliberately produces515 failures as a negative
control. Default tests/private/lifetime regressions and full build pass. Not
deployed; evidence `shadow-publish-0775` states remaining runtime/contract gaps.


M195 adds logical PTE copy groundwork:15935 core checks and6768 route regressions
pass, full dev build passes. COPY_PAGE_TABLE_ENTRIES is not runtime integrated;
see `shadow-copy` evidence for virtual-address and overlap constraints.


M196 composes staged GPU PTE-copy packets (34 DWORDs,48 aligned capacity), with
8493 host packet/routing checks passing.208 interpreted copy combinations match
snapshot semantics. No runtime integration; see `pte-copy-packets` evidence.


M197 (candidate0776) reserves private 4 KiB staging storage before engine stages
and adds a single-range copy builder with logical GPU VA resolution, table bounds
and accumulated DMA/ring capacity checks.26 storage and8552 packet/routing checks
pass, as do the actual lifetime test and full WDK build. The unlocked lifetime
control fails as expected. No deployment or hardware execution. DDI array/multipass
handling and accepted logical-copy publication remain to be integrated; CPU locking
does not prove GPU retirement. See `pte-copy-storage-0776` evidence.


M198 (candidate0777) wires COPY_PAGE_TABLE_ENTRIES dispatch to range-list building,
with range-index multipass and logical publication after DMA/private acceptance.
Each accepted range updates the mirror before the next is resolved. A40-range
chain across3 buffers passes, including exact private-record coverage and refusal
nonpublication.8654 host checks pass; omitted update/copy commits cause588 failures.
Full build/sign passes; not deployed. The host harness executes the list helper,
not the outer DDI dispatcher, and mocks address translation in new copy cases.
Internal refusal statuses propagate rather than become empty success, but remain
outside the documented restricted DDI return contract. Actual OS serialization,
GPU coherency/retirement, cache policy, reset/reentry and hardware acceptance are
still open. Evidence: `copy-publish-0777`; this supersedes M197's unwired-copy state.


### Copy ranges that change subsequent address resolution (host acceptance)

Hypothesis: a first accepted PTE copy can remap the source VA used by the second
range; that second command must name the new physical page before GPU execution.
Initialize a four-level hierarchy with CPU_VIRTUAL updates, map table VAs to
separate physical pages, and run the actual copy-list builder with the actual
logical walker. Compare the encoded second source with the independently chosen
new page; the live table must remain unchanged until a host packet interpreter
applies the copies. Run the omitted-publication negative control. This checks
construction and packet data flow, not real GPU timing or OS callback concurrency.


M199 verifies copy-range address dependencies with the actual logical page walker,
using a CPU-initialized four-level hierarchy. The first range remaps the second
range's source from page6 to page7; the second command encodes page7 before GPU
execution. Live RAM stays unchanged until host-modeled copies execute.8676 checks
pass; the omitted-publication control produces592 failures, including4 from this
new scenario. No KMD/package change, deployment, OS concurrency or hardware claim.
See `copy-real-walk` evidence. M198's broader list case still uses mock translation.


M200 expands the cache review with a concrete conditional source path: successful
POST WC mapping plus diagnostic BAR0 NC mapping of the same physical page. The
POST mapping's successful cache choice is not retained. This is separate from
M190's unresolved OS-owned aliases; it is not evidence that BAR0 and the carve-out
are the same CPU physical address or that cache aliases caused earlier hangs.
`cache-map-inventory` pins source snapshots and ownership domains. A consistent
policy must cover diagnostic partial-page overlaps, Present/DCN/page-table views,
and the POST fallback choice while keeping MMIO and cached system pages distinct.
No implementation/deployment or live attribute measurement in this inspection.


### POST cache alias regression

Extract actual DisplayMapFramebuffer/DisplayUnmapFramebuffer and
VramMappingProtection into a host harness. Model successful WC, fallback NC and
both mappings failing; assert requested attributes for aliases, page-edge overlaps,
distinct physical windows, invalid extents and teardown. An always-NC mutation
must fail WC-alias cases. Compile the full candidate for integration/WDK checking.
No hardware mapping behavior is established by this host model.


M201 (candidate0778) fixes the M200 diagnostic selection path: successful POST
cache attributes are retained and used by Vram Access and framebuffer sampling.
The shared selector compares physical pages, preserves NC for disjoint ranges,
and refuses mixed-domain ranges instead of extending WC onto unrelated pages.
2066 extracted mapping/policy checks pass; always-NC mutation fails2048. Full
build/sign passes; not deployed. Mapping callers are compiled/source-reviewed,
not host-executed. Other Present/DCN mappings and M190 OS-owned alias policy remain
open; this is not hardware cache/coherency or black-screen acceptance.
Evidence: `post-cache-0778`.


### Surface mapper integration

Extend the POST cache test with the actual VramMapCpuRange wrapper. Capture the
OS mapping flags for WC and fallback NC, preserve read-only access, and verify
invalid access/ranges and mixed ownership never reach the OS mapper. Compile all
Present/DCN call sites; their GPU behavior and callback lifetime are not covered
by this host test. Existing bounds checks and unmap ownership must remain intact.


M202 (candidate0779) routes six Present/DCN surface mappings through the shared
POST-aware VramMapCpuRange wrapper. It preserves read-only/read-write access and
rejects invalid or unresolved protection before calling the OS mapper.2072 host
checks pass; always-NC mutation fails2049; full build/sign passes. Call sites are
compiled/source-reviewed, not dynamically covered. No deployment. M190 VidMm
OS-owned alias policy, table/probe/private-pool mappings and hardware coherency
remain separate unresolved work. Evidence: `surface-cache-0779`.


### Dedicated page-table segment address groundwork

Before changing WDDM segment enumeration, verify the PTE translator can distinguish
application-local and table-local offset origins. Exercise512 pages in each address
unit mode and each entry kind; independently decode expected physical identities.
Check disabled second segment, end bounds, overflow, ambiguous IDs and alignment.
Existing one-segment regressions must remain unchanged. This is address groundwork,
not proof that a dedicated segment alone resolves borrowed OS mapping cache types.


M203 adds address groundwork for a dedicated local page-table segment. Optional
context fields distinguish its ID/base/extent from application VRAM for both
PDEs and leaf mappings; size0 preserves existing behavior.2048 address/unit/kind
cases and invalid-layout controls pass, along with8676 routing regressions and a
full dev build. Not configured or OS-advertised. Segment enumeration, root/table
bounds and walkers must change together before use. Isolation alone does not prove
borrowed CPU_VIRTUAL mapping cache attributes; M190 remains open. Evidence:
`table-segment-addresses`. Dev package retains0779 and must not be deployed.


### VidMm separate table extent

Test actual VidMmStartLayout with disjoint application/table extents. Capture the
mapped physical range and ensure only table storage is retained; CPU initialize
separate hierarchy, resolve application and table leaves, and refuse application
segment roots. Verify invalid layouts fail before pool/map allocation and stop
releases the table mapping. Existing single-segment wrapper remains until WDDM
layout enumeration is updated. This does not validate OS-owned cache attributes.


M204 adds VidMmStartLayout with separate application/table extents. Retained CPU
mapping and directory walks cover table storage only; roots and GPU update
destinations require its ID, while leaf bounds accept either local segment.
8693 host checks and full dev build pass. WDDM still calls the identical-range
VidMmStart wrapper, so enumeration/runtime layout is unchanged. Next integration
must select table capacity, split advertised segments and switch startup together.
Borrowed CPU_VIRTUAL mapping cache types/registration bounds and OS application
aliases remain unresolved. Evidence: `vidmm-table-extent`; dev package not for use.


### WDDM table segment layout integration

Candidate0780 advertises application1/aperture2/table3 and shares one layout
calculation between enumeration, table descriptors and VidMm startup. The table
budget is1/32 of usable VRAM rounded to64KiB, minimum4MiB; this is a policy choice,
not a hardware limit. Test disjoint/complete extents, framebuffer/tail exclusion,
small/disabled devices and overflow. Test CPU_VIRTUAL foreign physical identity
refusal before writes/registration. Compile full WDK integration. No lab deployment
until OS-owned cache attributes and other outstanding runtime gates are resolved.


M205 (candidate0780) wires the isolated layout into QUERYSEGMENT4, all page-table
level descriptors and WddmStart. Application1/aperture2/table3 use one shared range
calculation; ordinary allocation masks exclude3. Table budget is1/32 usable VRAM,
64KiB rounded, minimum4MiB.8922 host checks include actual two-pass descriptor
functions and stride guards; full build/sign passes. CPU_VIRTUAL physical identity
must fit the table extent before writes/registration. Not deployed. This removes
application/table overlap from the local source layout, not unknown cache aliases
with OS-owned table/application mappings. Runtime placement, pressure and capacity
acceptance remain open. Evidence: `wddm-layout-0780`; M190 is not closed.


### Diagnostic IB physical copy

Extract actual ProbeIb wrapper/walk and test a CPU-initialized system leaf.
Capture MmCopyMemory address/length/flags and verify page-tail bounds, failure and
short-copy output clearing, post-stop refusal and balanced lifetime lock. Assert
no additional MmMapIoSpaceEx calls during the probe. This does not test OS cache
selection or GPU snapshot consistency. Full WDK build checks API integration.


M206 (candidate0781) removes new NC aliases from optional IB diagnostics. ProbeIb
walks the retained table map under the shared lifetime lock and reads OS system
RAM through MmCopyMemory physical-copy mode. Exact status/byte count is required;
short/error reads clear the diagnostic result.8939 host checks and full build pass;
not deployed. This removes a concrete diagnostic mapping hazard but does not settle
M190 borrowed CPU_VIRTUAL cache attributes or application aliases. It also does not
provide a GPU-consistent snapshot or prove OS page lifetime. See `probe-copy-0781`.


### Physical page-list address groundwork

Verify a portable resolver for non-contiguous MDL PFNs and contiguous/non-contiguous
ADL-style lists. FirstPage is distinct from byte progress. Exhaustively compare
17 permuted page identities, every valid starting index and33 within-page offsets;
check page/count/overflow boundaries and zero outputs on refusal. Do not CPU-map
an ADL address: it may be an IOMMU logical address. The WDDM2.0 Transfer arm uses
MDLs, while MapApertureSegment2/ADLs belong to newer interfaces. DDI adapters and
packet publication are not yet connected by this helper.


M208 adds a portable page-list address resolver with separate first-page and byte
progress, fragmented/contiguous lists and overflow/page-bound checks.20196 address
checks plus13 boundary controls pass; existing stream and8939 routing regressions
pass. Not DDI integrated. The existing Transfer arm uses MDLs, while ADL-based
MapApertureSegment2 is newer. TransferOffset does not apply to MDLs; SegmentAddress
already includes the segment base. ADL addresses may be IOMMU logical and cannot
be treated as CPU physical mappings. See `page-list-addresses` evidence.


### MDL address adapter

Extract actual GfxPagingMdlAddress with modeled WDK MDL macros. Verify PFN array
indexing, separate first-page/byte progress, partial first/last-page byte bounds,
overflow and zero outputs on refusal. ByteOffset is relative to PFN[FirstPage];
MDL ByteOffset defines valid bytes rather than being silently added at every page.
This helper does not decide whether a non-page-aligned MDL can occur in the physical
Transfer DDI, pin pages, CPU-map them or publish GPU work. Compile against real WDK.


M209 adds GfxPagingMdlAddress over the actual WDK MDL access macros, bounding each
slice by ByteOffset/ByteCount before page-list resolution.21 new cases cover partial
first/last pages, PFN indexing and overflow;8960 total host checks and full dev build
pass. It borrows the MDL without mapping/locking/freeing it and does not add the
segment-only TransferOffset. Not DDI integrated; OS input reachability and lifetime
remain unverified. See `mdl-addresses` evidence. Dev package retains0781, not for use.

### 64-bit portable transfer progress (M210 procedure)

Hypothesis: the portable page splitter can preserve byte coverage and resume
positions above 4 GiB without changing its 32-bit callers or packet-size limits.
Add PagingStreamBuild64 and retain the old interface as a checked-width wrapper.
Run run_paging_stream.ps1 using MSVC host and kernel flags, checking every packet
of a transfer larger than 4 GiB, unaligned copy and aligned fill across the carry,
zero-capacity resume, overflow rejection and failure rollback after tentative output.
Run the existing KMD routing suite as a compatibility control. Failure is any
truncated address/progress, missing or repeated byte, changed legacy result or
published partial output on address failure. This does not establish the encoding
of WDDM's 32-bit MultipassOffset or integrate physical Transfer into the DDI.

M210 result: PagingStreamBuild64 passes exact packet coverage above 4 GiB,
copy/fill carry and refusal controls. Legacy stream tests and 8960 routing checks
pass; host and kernel-flags compilation pass. Evidence: `stream64`. No full driver
package built or deployed. External DDI resume encoding remains pending.

### Physical endpoint packet construction (M211 procedure)

Hypothesis: independent source/destination resolvers can construct the existing
SDMA map/copy/unmap transactions for two different MDLs at identical byte offsets,
without conflating their PFNs, while retaining local MC and virtual routes.
Introduce endpoint-aware 64-bit streaming and a physical builder under the existing
GfxPagingLock. Local endpoint addresses are already MC addresses, bounded by the
advertised endpoint extent and device VRAM; MDL progress is separate from FirstPage.
Host tests extract the actual builder/resolvers/emitter and inspect distinct PFNs
and source/destination GART slots for fragmented MDLs over multiple buffers. Check
local/local, both mixed directions, fill, capacity, invalid extent/PFN and not-ready
rollback. Existing virtual routes and portable >4GiB tests are compatibility controls.
Compile the full driver against WDK26100. These tests do not prove OS MDL lifetime,
GPU coherency or DDI delivery. External segment selection, transfer flags and UINT
MultipassOffset encoding remain WDDM adapter responsibilities, not implicit defaults.

M211 result: independent source/destination callbacks and GfxPagingBuildPhysical
now drive the existing direct/GART packet emitter with bounded MDL/local endpoints.
40 new checks pass,9000 routing total; portable wide/legacy tests and full WDK dev
build pass. Initial test underestimated aligned reservation (96 for83 DWORDs);
correcting its capacity to180 permits the intended two transactions. Both logs are
preserved in `physical-stream`. No DDI wiring or lab deployment; dev package retains0781.

### Whole-range physical preflight (M212 procedure)

Hypothesis: invalid physical endpoint extents must fail before the first packet,
including when the first DMA buffer is too small to reach the invalid tail.
Add a constant-time whole-transfer extent preflight to GfxPagingBuildPhysical.
Test MDL described bytes and FirstPage, local VRAM end and arithmetic/tag overflow,
with small command capacity and untouched payload canaries on refusal. Retain valid
exact-end and resumed controls. Per-page PFN checks remain necessary; this is not
an up-front scan of every PFN or proof of OS lifetime. Run actual extracted route
suite and full WDK build, preserving prior route/wide-stream tests as regressions.

M212 result: complete endpoint byte extent is checked before packet construction,
including MDL FirstPage/ByteCount and device VRAM/tag/arithmetic limits.11 new
checks pass,9011 total; full WDK dev build passes. Initial C4127 condition warning
was corrected without disabling warnings. Evidence: `physical-preflight`. No
portable stream changes or rerun; no DDI integration or lab deployment.

### Physical Fill DDI integration (M213 procedure)

Hypothesis: physical Fill can select a bounded local segment and publish ordinary
per-buffer SDMA/private records with a UINT page-progress token, including an
unaligned first page and total sizes above4GiB. Wire DXGK_OPERATION_FILL to a
shared-layout endpoint selector and physical builder. Token counts completed page
slices; the first slice ends at the next destination page boundary, the last may
be short. Test actual adapter and publication with modeled WDK fields, small
buffers, command/private canaries, independent address/pattern/progress checks,
invalid segment/extent/private capacity, and resumed >4GiB fill. Run routing and
full WDK build. No lab deployment until open hardware/cache/error gates are resolved.
Physical Transfer, aperture destinations and OS failure policy remain separate work.

M213 result: physical Fill DDI calls WddmBuildPhysicalFill, selecting local segment1/3
through WddmMemoryLayout. UINT tokens count page slices, including partial first/last
pages; bytes moved remain64-bit.64 new/9075 total checks and full0782 build pass.
Actual adapter/publication host-tested; outer DDI branch compile/source only.
Evidence physical-fill-0782 preserves failures and final pass. Not deployed.
Logical shadow synchronization for filled table storage remains required, alongside
aperture operations, physical Transfer and the restricted OS error policy.

### Physical table Fill shadow commit (M214 procedure)

Hypothesis: accepted physical table fills update registered logical PTEs before DMA
publication, preserving untouched DWORD halves and never reading unknown entries.
Add a bounded physical fill to the shadow, an exclusively locked VidMm commit and
publication preflight before that commit. Test full/half PTEs, unknown entries,
multiple tables, invalid-range rollback and actual Fill adapter publication/refusal.
Run shadow and routing suites and full WDK build. Unknown half entries may remain
conservatively unknown; no fabricated zero or implicit table registration is allowed.
Virtual Fill and arbitrary copies into table storage remain separate integration work.

M214 result: PagingPtShadowFill, exclusive VidMmCommitPagingFill and physical Fill
publication now keep registered table construction state ordered with accepted
commands.70999 shadow/9082 routing checks pass; omitted update/copy/fill commit
mutation fails596 as expected. Full0783 build passes, not deployed. Evidence:
fill-shadow-0783. Partial unknown entries stay conservatively unknown; separate
half writes do not accumulate knowledge. Virtual Fill and other table-write routes
remain separate work. No live GPU/OS ordering acceptance.

### Partial PTE knowledge (M215 procedure)

Hypothesis: two accepted DWORD fills of an initially unknown PTE must make its
complete value readable, including when a partially known PTE is copied first.
Track two known bits per PTE, require both for Read, and propagate both through
Apply/Copy/Fill. Test both fill orders across all512 PTE positions, partial source
copies, unknown replacement and overlapping copies against an independent snapshot
model. Existing full-entry, fill/publication and kernel/full-build tests must pass.
Unknown storage placeholder bits are never exposed as known data.

M215 result: two known bits per PTE allow separate DWORD fills to produce a complete
entry and preserve partial knowledge through copies. Read requires both bits.
112471 shadow checks (41472 new),9082 routing checks and full0784 build pass.
Evidence: half-pte-0784. Adds64 bytes per table slot; no lab deployment. Supersedes
M214 conservative treatment of separate half writes, not open virtual-fill/GPU gates.

### Virtual Fill ordered publication (M216 procedure)

Hypothesis: building and publishing one page slice at a time preserves the resolved
physical destination for table-shadow commit and lets later slices observe accepted
table changes. Add a fill-page builder returning its exact physical identity, then a
virtual Fill adapter publishing each slice before the next translation. Keep input
DmaBufferWriteOffset unchanged on return while accounting for accumulated packets.
Test actual page builder/adapter with local/system destinations, table-shadow change,
capacity refusal, exact addresses and token progress. Existing packet/routing tests
and full WDK build are required. This does not resolve OS construction concurrency,
virtual transfer writes to tables or the restricted outer failure contract.

M216 result: GfxPagingBuildFillPage captures exact local/system physical identity;
WddmBuildVirtualFill publishes and commits tracked table effects per slice before
building the next.24 new/9106 checks pass; omitted logical commits fail599; full0785
build passes. Evidence: virtual-fill-0785. New tests use modeled translation, not a
real-walker self-remapping scenario. Virtual byte-token size limit remains32-bit;
no deployment or OS/GPU ordering claim.

### Virtual Fill actual-walker dependency (M217 procedure)

Hypothesis: after a virtual Fill publishes a zero-fill of its own leaf table,
the next virtual page must fail logical translation while the retained live table
still resolves until modeled GPU execution. Initialize an actual four-level table
through CPU_VIRTUAL DDI helpers using the advertised split layout. Map VA10000 to
the leaf table and VA11000 to a different page. A zero fill of8192 bytes must publish
only4096 bytes, preserve the accepted prefix and refuse the next translation.
First test insufficient command capacity leaves both logical and live maps intact.
Inspect packet identity/private coverage and independently apply its decoded fill
to the host memory model; only then should the live walk stop resolving. Run normal
and omitted-logical-commit controls. No hardware execution or OS self-mapping claim.

M217 result: actual four-level logical walker sees the accepted zero fill of its
leaf table and refuses the next slice, retaining exactly one published page/packet.
Live retained mapping remains valid until independent modeled packet execution.
21 new/9127 checks pass; omitted logical commits fail602, including3 new dependency
checks. Evidence: virtual-fill-real-walk. No production source/package change,
full-build rerun, lab access or claim that real OS traffic uses this self-mapping.

### UINT slice-progress codec (M218 procedure)

Hypothesis: a UINT count of completed page slices can round-trip the stream's64-bit
byte progress for differently aligned source/destination endpoints without storing
intermediate state. Compute the union of periodic source/destination page boundaries,
with zero as initial state and a terminal token for a partial last slice. Preflight
token capacity for the whole range. Compare against actual one-packet stream progress
across many alignments and resumed >4GiB cases; test exact token capacity, overflow,
non-boundary progress and refusal outputs. Run portable/kernel compilation and routing
regressions. Codec integration into the physical Transfer DDI remains a separate step.

M218 integration procedure extension: use the common codec for physical and virtual
Fill, replacing physical inline token arithmetic and the virtual32-bit byte cap.
Update virtual Fill progress assertions to slice tokens; add a >4GiB resumed actual
adapter test with only the final slices built. Re-run routing/commit mutation controls
and full WDK build. New candidate requires normal device restart; do not mix token
formats within an existing paging request or deploy it into an active session.

M218 result: stateless slice tokens round-trip12405 actual stream boundaries and
wide/capacity controls. Both Fill adapters use the codec; virtual Fill byte-size cap
removed, with an actual adapter test of the three-slice tail crossing4GiB.9129 routing
checks and full0786 build pass; omitted commits fail602. Evidence: paging-token-0786.
Initial header/type compile errors corrected without disabling warnings. Not deployed;
virtual token formats cannot be mixed across versions within an in-flight request.

### Physical Transfer argument preparation (M219 procedure)

Hypothesis: WDK Transfer arguments can prepare two independently bounded endpoints
and resume state without conflating segment TransferOffset and page-based MdlOffset.
Share the existing full endpoint preflight with preparation. Validate all flags;
Swizzle/Unswizzle remain unsupported rather than being silently copied linearly.
Test MDL/local, local/MDL, two distinct MDLs, local/local, segment bounds, first-page
bounds, preserved resume under TransferStart/End, reserved flags and cleared outputs
on refusal. Extract actual preparation in the route harness and compile full WDK.
This stage does not publish Transfer commands or resolve table-copy shadow effects.

M219 result: WddmPreparePhysicalTransfer uses shared endpoint extent validation and
the slice codec.43 new/9172 checks and full WDK dev build pass. Tests preserve
independent MDLs and segment offsets, validate flags and resume state. Initial host
C4201 warning resolved by local pragma around WDK-style anonymous-union declarations;
full WDK compile unchanged. Evidence transfer-prepare. Dev package retains0786, not
for deployment; physical Transfer publication and table effects remain pending.

### Byte-granular logical copy (M220 procedure)

Hypothesis: arbitrary byte copies can preserve known source bytes and invalidate
unknown ones without fabricating complete PTEs, including overlapping ranges.
Track knowledge per byte and add one-page-slice copy metadata operation. Validate
all bounds before mutation; copying untracked source clears destination knowledge.
Test independent byte snapshots over source/destination offsets, overlap directions,
missing/partial source, assembling a PTE from single-byte writes, bitmap/page bounds
and unchanged existing Apply/Copy/Fill behavior. Run shadow/routing suites and full
WDK build. This helper must not imply SDMA overlap ordering without corresponding
hardware packet staging; physical Transfer DDI publication remains separate.

M220 result: per-byte Known masks and PagingPtShadowCopyBytes pass784240 shadow
checks plus9172 routing regressions and full0787 build. Snapshot oracle covers
unaligned byte ranges with mixed knowledge, overlapping/self copies and completing
missing bytes; PTE Copy retains partial-byte knowledge too. Evidence byte-shadow-0787.
Adds384 bytes/table slot versus0786. Helper not Transfer-DDI wired; GPU copy overlap
must match metadata ordering before use. No deployment or hardware claim.

### Staged byte-copy packets (M221 procedure)

Hypothesis: the existing two-copy/two-barrier staging transaction supports byte
ranges without PTE alignment, preserving snapshot semantics for one bounded slice.
Generalize emission to1..4096 bytes and keep the PTE entry-count/alignment wrapper.
Compare actual two COPY_LINEAR payloads against independent memmove across unaligned
source/destination offsets and lengths, including overlap/self-copy. Inspect markers,
capacity atomicity, tail guards, staging/marker aliases and invalid size/overflow.
Run routing packet regressions and full WDK dev build. This does not prove GPU barrier
timing, cross-slice overlap direction or GART mapping lifetime; no DDI integration yet.

M221 result: bc250_sdma_paging_copy_bytes generalizes the existing staged transaction;
PTE helper retains entry/alignment validation and forwards to it.2349 new/11521
packet/routing checks and full dev build pass. Evidence staged-bytes. Same48-DWORD
reservation/34 emitted, two copies/two barriers. Independent decoded memory model
matches memmove; actual GPU timing and multi-slice overlap ordering remain unverified.
Dev package retains0787 and must not be deployed.

### Physical copy slice identity (M222 procedure)

Hypothesis: a one-page physical copy builder can return exactly the resolved source/
destination identities only after complete packet construction, with atomic refusal.
Use existing endpoint resolvers under GfxPagingLock. Disjoint local uses direct copy,
local overlap uses reserved staging, system endpoints use existing mapped packets.
Until mapped staging is implemented, aliasing system slices must explicitly refuse.
Test identities against PFNs/MC conversion, local overlap staging, mixed directions,
capacity/missing-stage/page-boundary refusal and output clearing. Run actual extracted
builder/routing suite and full WDK build. No full-transfer overlap or DDI claim.

M222 result: GfxPagingBuildCopyPage returns captured BC250_PAGING_COPY_SLICE identities
only on complete construction.16 new/11537 checks and full WDK dev build pass.
Local overlap stages; direct and GART routes preserve identity. Aliased system pages
refuse pending mapped staging. Evidence copy-slice. Dev package retains0787, not for
deployment. Transfer publication, shadow commits and cross-slice ordering remain open.

### Mapped staged copy (M223 procedure)

Extend mapped transfer with optional private staging, keeping both PTE mappings alive
through two copies and their barriers. Preflight the complete transaction and stage/
marker/table aliases. Test aliased PFNs through distinct GART slots, exact packet
order/markers/unmap, capacity canaries and a decoded memmove data model. Re-run routing
and full WDK build. This addresses one slice, not arbitrary cross-slice overlap.

M223 result: optional staging_mc extends mapped copy to100 emitted/112 reserved
DWORDs. Both mappings survive the two copies/barriers, then cleanup. KMD stages
aliased system slices.11661 checks and full0788 build pass; evidence mapped-stage-0788.
No deployment or actual GPU cache/retirement, OS lifetime or cross-slice ordering claim.

### Physical Transfer publication (M224 procedure)

Wire prepared Transfer endpoints to per-slice packet construction and logical byte
commit after private-header acceptance. Token counts completed slices; reverse local
overlap visits suffix slices first and forces staged barriers even for disjoint
individual slices. Test actual adapter/publication and table metadata, both mixed
directions, insufficient buffers, reverse order and independent packet data model.
Different multi-page MDLs require alias analysis; refuse this still-unimplemented
case before any output rather than assume no cross-page aliases. This is a temporary
coverage gap, not a final contract. Run routing/mutation controls and full WDK build.

M224 result: physical Transfer DDI is wired to prepared endpoints, captured slice
identities, header preflight, tracked-table byte commit and DMA publication. Whole
local overlap direction is handled with forced staging/barriers for every slice.
11700 checks and full0789 build pass; omitted logical commits fail605, including
three new Transfer assertions. Evidence transfer-publish-0789. Tests use modeled OS
fields; outer DDI branch compile/source only. Distinct multi-page MDLs remain refused
pending cross-page dependency analysis; mixed alias/lifetime/error-policy gaps remain.
No deployment or hardware acceptance.


### Paging contract scope review (M225)

Compare the BuildPagingBuffer remarks with the argument reference before adding
arbitrary MDL dependency scheduling. Record explicit guarantees separately from
omitted cases; make no hardware inference. Result: aperture Fill is explicitly
excluded by VidMm, while the inspected sources do not settle two-MDL reachability
or cross-page non-overlap. M224's guard remains unresolved. Evidence:
[transfer-contract-review](../../evidence/windows/2026-09-23-E27-m9-inference/transfer-contract-review/).
No code change or test run; virtual Transfer table effects and required aperture
map/unmap are next implementation priorities.


### Virtual copy slice identities (M226)

Procedure: share physical/virtual slice emission, resolve virtual addresses under
the engine lifetime lock, then test packet/identity agreement, aliases, forced
staging and refusal atomicity. Build with WDK. Expected: returned identities match
the packet endpoints and no refused slice exposes successful metadata.
Result:11712 checks pass and full dev build passes; evidence virtual-copy-slice.
Virtual translation controls use the host model. DDI publication and actual-walker
table dependency tests remain next; no deployment or hardware acceptance.


### Virtual Transfer publication (M227)

Procedure: integrate captured virtual-copy identities into table commits before
publication; use wide slice tokens and test actual walker dependency, multipass,
header/capacity refusal, and a modeled >4GiB tail. Expected: accepted copy changes
logical state before the next translation; refused copy preserves it. Run the
omitted-commit control and full WDK build.
Result:11746 checks pass, omitted commits fail608 (3 new dependency assertions),
full0790 build passes. Evidence virtual-transfer-0790. Actual source/host behavior
only; outer DDI compiled. No deployment, GPU acceptance or cross-page alias claim.


### OS aperture partition (M228)

Before map/unmap writes, verify a permanent OS range excludes driver GTT and
temporary paging slots. Add pure layout/range functions and independent numeric,
last-page, short backing, alignment and overflow controls; build with WDK.
Result:86 new/11832 checks pass and dev build passes; evidence aperture-partition.
Helpers are not yet advertised or mapped. Descriptor/runtime geometry integration,
ordered PTE writes and hardware validation remain next.


### Captured aperture descriptor (M229)

Procedure: capture bounded aperture geometry under GartLock through existing
setup, retain it for the device start and use it in QuerySegment4. Test a nonzero
GART base and failures, requiring matching descriptors and cleared outputs.
Result:11839 checks and full0791 build pass; evidence aperture-layout-0791.
Actual capture/query code uses modeled setup/lock controls. Startup integration
is compiled/source inspected; no deployment or runtime aperture claim.


### Permanent aperture packet transaction (M230)

Procedure: reserve PTE writes, memory barrier and GART invalidation together;
compare with separate existing constructors and validate payloads, repeated
nonzero dummy PTEs, capacity atomicity, marker separation and address bounds.
Result:1795 new/13634 checks and full WDK dev build pass. Evidence aperture-packets.
Not DDI integrated, deployed or hardware validated.


### Map/unmap aperture DDI (M231)

Procedure: connect M230 packets to captured geometry, MDL page indices, cache
flags and repeated DummyPage; bound batches by ring/private capacities and advance
resume only after publication. Test both operations, nonzero base, partial MDL
pages, fragmented PFNs, multipass and refusal controls. Full WDK build checks ABI.
Result:44 new/13678 checks and full0792 build pass; evidence aperture-ddi-0792.
Ready-engine host behavior only; outer DDI compiled. Pre-RUN OS map delivery and
hardware/lifetime/cache acceptance remain unresolved. No deployment.


### Startup ordering review (M232)

Inspect StartDevice, subsystem preparation, GART enable, firmware loading, engine
readiness and the prior successful manual sequence before adding bootstrap maps.
Result: full table zeroing would erase an earlier direct map; kernel firmware
loading already exists, while automatic startup and safe partial-failure unwind
remain work. E16 run009 observed no map call, so early-map delivery is not claimed.
Evidence startup-contract-review; implementation/acceptance plan in
[WDDM startup](../../docs/design/wddm-startup.md). No code or lab changes.


### Stop dependency failure injection (M233)

Procedure: execute actual Fini/GfxStop/PspStop/GartStop against modeled hardware
responses across128 failure combinations and manual FINI cases. Require PSP/GART
retention after an unconfirmed consumer, no early sequence-page release, correct
positive-case release and no loss of verdict on repeated stop. Compare prior code.
Result:1676 checks pass; prior functions fail889. Full0793 build passes; evidence
stop-chain-0793. Hardware reset and containment beyond one device object remain
unverified. No deployment or lab access.


### Shared initialization entry points (M234)

Procedure: extract command implementations without changing their bodies; route
diagnostic and internal startup entry points through them. Execute actual new
wrappers with modeled outcomes and real report declarations; require preserved
native errors/partial reports and refusal of incomplete success. Mutate completion
handling and require regression detection. Full WDK build checks production ABI.
Result:four bodies unchanged,141 checks pass, mutation fails30, full dev build
passes. Evidence init-entrypoints. No startup orchestration or hardware execution.


### Firmware preflight ownership (M235)

Procedure: prepare and validate the existing firmware inputs before hardware,
retain exact bytes for later borrowed PSP execution, and inject every file/pool
failure plus malformed layout and failed load. Require complete cleanup before
hardware, no re-open during prepared loading and explicit caller ownership.
Result:32 new controls and141 initializer regressions pass; full dev build passes.
Evidence firmware-preflight. Parser/I/O/hardware are modeled; coordinator remains
unconnected and no lab run is claimed.

### Startup coordinator controls (M236, planned)

Hypothesis: the unpublished-device coordinator prepares firmware before writes,
records partial phases, unwinds every attempted hardware start in dependency
order, and refuses success without both local and system paging resources.
Run build-startup-coordinator-test.cmd: extracted coordinator and readiness code
with modeled subsystem APIs. Fail each phase, resource and final readiness;
verify report preservation, stop order, firmware ownership and no publication.
Omitting IH stop or final readiness must fail. These are host controls, not
interrupt timing or hardware reset evidence. Automatic PnP activation stays off.

M236 result:291 host checks pass; omitted IH stop fails11, omitted readiness
fails3. Stop1676 and firmware32 regressions pass. Full WDK dev build passes
(SYS85DA7A17A1F3A82505F29DD2C8AB0D48EBB182142CCC1713198E026C891B91B3).
[evidence](../../evidence/windows/2026-09-23-E27-m9-inference/startup-coordinator/).
No deployment; automatic PnP activation remains off pending integration review.

### M237 interrupt preflight

Require native synchronization success, callback delivery and TRUE result before
hardware attempts. Extended extracted startup controls:319 pass; generated
--omit-interrupt-preflight mutation333checks/7failures. Full WDK dev build passes.
[Evidence and local MS snapshots](../../evidence/windows/2026-09-23-E27-m9-inference/interrupt-preflight/).
Actual interrupt delivery and IH publication transition remain unverified.

### M238 IH split preparation (planned controls)

Separate the existing AMD sequence before its final enable write. Preparation
must leave both ring/interrupt enable bits clear at every write; final enable
adds exactly one write. Compare the concatenation to the independent E03 Linux
trace including allocated address controls, then run existing IH decode/fence
controls and WDK build. This is a prerequisite for synchronized KMD publication,
not evidence that the existing KMD race is fixed.

M238 result: prepare14+enable1 writes match E03 Linux trace; zero mismatches and
zero address failures. Existing IH controls and WDK build pass.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/ih-split/).
KMD still uses combined API; synchronization integration remains next.

### M239 IH publication controls (planned)

Extract actual publication helper, synchronized callback and ISR. Model native
synchronization refusal, missing callback, false result, MMIO/shim failure and
immediate ISR after callback. Verify consumer readiness at enable, owned callback
report storage, caller log bounds and backend restoration. Late Active mutation
must fail. Full KMD build covers integration; hardware delivery remains untested.

M239 result:95 checks pass, late Active mutation fails7. Initializer141 and
startup319 regressions pass; full WDK dev build passes.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/ih-publication/).
INIT integration compiled; existing Fini cleanup not simulated by these controls.
No deployment or actual hardware interrupt measurement.

### M240 IH stop admission controls (planned)

Extract actual synchronized close, Fini and IhStop. Inject synchronization native
failure, no callback, false result, unavailable device and nonquiet hardware.
Model an ISR that has read Active but queues its DPC only at synchronization.
Verify join/flush before teardown, retained owner/backing and sticky quarantine.
No-sync mutation must fail; no runtime hardware-retirement claim.

M240 result:177 checks pass; no-sync mutation171checks/54failures. Activation95
and dependent stop1676 regressions pass; WDK dev build passes.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/ih-stop-admission/).
No live interrupt, hardware halt or reload-containment proof.

### M241 WDDM automatic startup integration controls (planned)

Extract actual WddmStart and diagnostic allowlist. Inject both pool failures,
missing GPU VA/layout, GART geometry failure, partial VidMm initialization and
GPU startup failure. Verify no publication/leaks and exact cleanup ownership;
success must publish after CPU resources and GPU readiness. Omitted startup or
VidMm cleanup must fail. Diagnostic mutations (including PLAN) refused under
FullWddm; observational commands and GFX/IH STATE permitted. Host tests model
OS resources and hardware; lab deployment requires new startup-aware scripts.

M241 result:89 checks pass; omit-startup87checks/8failures and omitted-cleanup
89checks/4failures. Coordinator319 regressions and WDK dev build pass.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/wddm-autostart/).
Not deployed. Old manual initialization scripts must be replaced before live testing.
Correction M242: the clock task uses independent bc250rd SMU IOCTLs and remains
compatible with the miniport escape guard; verify its startup ordering.

### M242 clock readback controls

Actual CheckClock with modeled SMU replies: matching1000/116 and1500/104,
frequency/VID mismatch, each query failure and out-of-range requests.10 scenarios
pass; removed comparison fails2; bc250rd build passes.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/clock-path-review/).
CLI not deployed. Service/task snapshot does not establish current clocks or
ordering before GPU startup.

### M243 live clock control

Hash-verified M242 CLI copied separately to target tmp. Positive1000/820 exits0,
negative expected1001/820 exits1, recheck exits0; actual1000MHz/VID116 throughout.
67C and STOP clear before test. No clock-setting messages or driver/task changes.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/clock-live-control/).
The [automatic-start trial plan](startup-live-plan.md) tracks deployment
preconditions, including one-shot startup containment and new scripts.

### M244 durable one-shot gate controls (planned)

The gate already exists: EnableFullWddm1 consumes to0,2 persists. Verify actual
GuardConsumeSetting plus WddmGateOpen with write/flush failures and simulated
loss of cached registry state at reboot. Failed durable closure must refuse,
even if caller default enables. Original source must fail these controls.

M244 result:40 guard/gate checks pass; original source fails7. WDK build passes.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/oneshot-durability/).
Use existing EnableFullWddm1 for the trial;2 persists. The gate is consumed per
DriverEntry/load. Runtime durability and repeated PnP remain separate checks.


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


### M252: inference advances to a distinct GFX timeout

Recovered0798 dump shows accepted GFX fence167 (IB5152bytes,sequence6496) failed to reach its fence before500ms; the slot held6495. Fence168 refusal follows that timeout. SDMA continued submitting after the initial GFX stall. This does not contradict the passing Vulkan/SDMA control or identify a node0 queue defect. Next isolate the failing inference IB/shader/mappings. See facts M252.


### M253: correct ICD selection and successful inference

The elevated0798 harness ignored environment-based ICD selection and used the registry m8 driver. This was a harness regression from ops-gpu-016, which uses a Limited interactive task. Read-only loader comparison confirms both paths. Reusing the identical0798KMD with the hash-matching quiet ICD through a Limited task passes Vulkan and one reference-matching run each of stories15M/TinyLlama. GFX2354/2354 andSDMA24161/24161,zero timeouts/refusals,noTDR. Automatic startup-to-inference is now verified; repeat/pressure/performance and the remaining audit contracts are still open. All future acceptance harnesses must capture the actual loader path and DLL hash. See facts M253.


### Repeat0798 acceptance plan

Run four fresh processes each of stories15M (96 tokens) and TinyLlama (64 tokens), alternating models in the existing full WDDM0798 session. Use the M253 Limited interactive harness, hash-verified quiet ICD, loader witnesses, full GPU offload, deterministic E14 prompts and settings. Require all native exits zero and all texts equal to Linux after CRLF normalization. Capture temperature, clock readback and final real hardware completion counters. No GPU reinitialization or Windows reboot. Failure of any run leaves repeatability unverified.


### M254: eight repeatable inference processes

Four fresh processes per model pass in the same active full WDDM0798 GPU session, alternating stories15M and TinyLlama. All eight native exits are zero, actual loader traces select the hash-verified quiet ICD, all layers are offloaded, and output equals the immutable E14 Linux reference after CRLF normalization only. Cumulative hardware counters: GFX11674/11674 and SDMA120766/120766; zero timeouts/refusals and no TDR. Clock readback1000MHz/VID116; observed temperature70.6-71.6C. No reinitialization or reboot. Repeat acceptance passes; equivalent benchmarks, actual GPU pressure/eviction and the remaining audit/lifecycle contracts stay open. See facts M254 and repeat0798 evidence.


### Bench0798 plan

Measure b9564 llama-bench pp512/tg128, r3, t6, ngl99 on both models in the same full WDDM0798 session after M254. Limited interactive harness, actual quiet ICD loader/hash witnesses, clock1000MHz/VID116 and temperature controls. Archive raw JSON and counters. Compare TinyLlama to E14 dpm/bench-1000.txt (same b9564 and workload); backend/OS differences remain explicit. The available stories15M Linux table does not establish equivalent1000MHz settings, so do not claim a like-for-like ratio from it.


### M255: full-WDDM benchmark baseline

Unchanged0798 and quiet ICD complete b9564 llama-bench pp512/tg128, r3, t6, ngl99 at verified1000MHz/VID116. TinyLlama:1092.70 +/-4.16 prompt tokens/s and113.88 +/-5.00 generated tokens/s. E14 Linux1000MHz reference:1119.59 +/-0.40 and154.92 +/-0.42 (Windows2.40%/26.49% slower). stories15M:37162.42 +/-426.03 and474.49 +/-110.83; generation samples350.30,563.32,509.85 show substantial spread. CumulativeGFX22039/22039 andSDMA150022/150022,zero timeouts/refusals,noTDR;temperature70.6-72.2C. No reboot/reinitialization. Linux and Windows differ in driver/backend stacks; Linux text table does not expose every default and no matched1000MHz stories15M reference is established. Profiling, actual GPU pressure/eviction and remaining audit contracts remain open. See facts M255.


### GPU residency positive-control plan

New gpu-residency-probe reuses BC2A allocation and bounded KMT helpers, adds BC2C/BC2S submission of the imported DMA_DATA packet sequence, and copies every source byte through the GPU into a sentinel-initialized <=1MiB readback buffer. A monitored fence must complete before every full CPU comparison. First run64KiB VRAM in current0798 session; require initial GPU control plus3Evict/dirty-pressure/MakeResident/GPU-readback cycles. Status2or3 proves departure from GPU-memory residency only; physical transfer needs kernel paging trace. No reinitialization. Scale only after small control succeeds. Build /W4 /WX and --help pass; live results pending.


### M256: real GPU readback through residency controls

New gpu-residency-probe uses BC2A/BC2C/BC2S and imported DMA_DATA definitions to copy every source byte through GFX into a sentinel-filled readback buffer, waits for monitored fences, then compares every dword. Built /W4 /WX; help smoke passes. VRAM64KiB: initial read and3Evict/dirty-pressure/MakeResident cycles pass, residency2->1 each cycle and4real GFX completions. Small harness omitted native numeric exit due Process.ExitCode handling; explicit final PASS/fence/readback witnesses are retained. Corrected cmd exit capture used for1GiB.

VRAM1GiB initial readback passes1024fragments; first Evict succeeds but residency stays1for5seconds, so native exit1 occurs before pressure allocation. Final cumulativeGFX23067/23067,SDMA201363/201363,zero timeouts/refusals/noTDR. This is not post-eviction1GiB acceptance or proof of physical relocation. Local MS d3dkmthk.md:12491 defines Evict as residency reference decrement. Probe now creates/dirty-fills competing memory before requiring departure; revised source builds but is not deployed. Next validate that revision, retain strict departure and GPU-content witnesses, then isolate actual transfer operations. See facts M256.


### GPU residency revision2 plan

Deploy the pressure-before-residency-query probe to a unique v2 directory. Run64KiB first with native exit capture; only on success run1GiB. Preserve strict departure from GPU-memory residency and full-range post-MakeResident GPU readback. Keep current0798 session and temperature/clock controls; collect before/after paging logs. Physical relocation remains a separate evidence requirement.


Revision2 small control passes exit0.1GiB reaches NOTRESIDENT after pressure, then tool MakeResident wait expires at5s; KMD counters show no hardware timeouts/refusals/TDR. Revision3 retains every GPU content check but removes redundant CPU reads of write-combined source/pressure, and gives the aggregate re-residency operation a measured60s tool deadline (individual GPU waits5s and KMD500ms watchdog unchanged). Run small then large with unique outputs. Longer waiting is diagnostic, not proof of a driver fix.


### M257: three1GiB residency cycles with full GPU readback

The revised probe passes64KiB and1GiB controls with native exit0. Each of3large cycles witnesses NOTRESIDENT3 after dirty competing1GiB memory, restores GPU-memory residency1, then verifies every dword through GPU CP DMA readback. Initial plus3post-cycle reads cover4GiB through4096real GFX submissions. Re-residency takes6282/6187/6171ms; earlier5s tool limits were too short for these observed operations. Aggregate map/residency tool waits are60s, individual GPU waits5s; KMD watchdog unchanged. Final cumulativeGFX30247/30247 andSDMA541409/541409,zero timeouts/refusals/noTDR. No reboot/reinitialization.

This closes the tested positive1GiB residency/content-preservation case, not all paging contracts. Physical endpoint tracing, shader cache visibility, GTT/aperture paths, arbitrary aliases, OS lifetime and recovery remain separate. Source/probe revisions2/3 and their failed waits are preserved. See facts M257 and gpu-residency0798-v4 evidence.


### GTT residency control plan

Use unchanged M257 probe/KMD in the active session with preferred_heap GTT. Run64KiB then64MiB only if the small control passes. Keep full GPU readback and three cycles; aperture capacity is256MiB, so64MiB source plus pressure/readback/IB fit. Report observed residency states without assuming local-VRAM behavior. Capture native exits and hardware counters. No reboot or initialization.


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


### Shader coherency probe procedure (2026-09-23)

Hypothesis: sixteen rounds of changed host-coherent inputs in the same Vulkan allocations are visible to two chained integer-mixing shaders, and their complete outputs are visible to the CPU after a fence. Build shader-coherency-probe.c with build-shader-coherency-probe.cmd; run under the verified quiet ICD in a Limited interactive task, using existing E14 inthash.spv. Every round compares all1M output words against two CPU mix32 applications. Intermediate/output sentinels differ from the expected data. The --stale-input-control deliberately omits round1's input rewrite and must exit1 with a mismatch; normal execution must complete16rounds/0mismatches. Enforce existing1000MHz/820mV and85C thermal limit. Record loader witness, hashes, full output and before/after driver logs. This tests shader visibility for reused host-coherent allocations, not eviction, NC/WC alias types or legacy aperture callbacks.


### M264: positive reused-buffer shader visibility on unit A

Unchanged full-WDDM0798 and verified quiet ICD pass two16-round processes: each round changes1M input words in reused host-coherent allocations, runs two chained integer shaders, and compares all output bytes with the CPU oracle. Intentional stale-input control detects round1 with the previous GPU hash retained. Native exits0/1/0 and independent host validation pass. GFX30541/30541,SDMA673837/673837,zero timeouts/refusals,noTDR;1000MHz,temperature71.0..71.5C. No reboot or driver deployment.

This adds shader visibility evidence beyond CP DMA, but does not identify cache attributes of borrowed page-table pointers or prove shader visibility after eviction, all memory types, or legacy aperture callbacks. See facts M264 and shader-coherency0798 evidence.


### Shader memory-type matrix procedure (2026-09-23)

M264 reused E14's selector, which prefers DEVICE_LOCAL among compatible HOST_VISIBLE|HOST_COHERENT types; it did not print each chosen allocation type. Revised probe adds --memory-type N and prints the actual selected type/heap/flags for all three buffers. On the observed RADV device test types2(flags0x6,heap0),3(flags0x7,heap1) and5(flags0xe,heap0), each positive16rounds, stale-input negative control, then positive repeat. Reject unsupported extension-property types rather than silently selecting another type. Native exits must be0/1/0 and stale mismatch must retain round0's GPU hash. Maintain Limited interactive task, actual quiet ICD witness,1000MHz and85C limit. These Vulkan properties do not reveal the cache attributes of independent OS page-table pointers.


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


### Candidate07100 paired cache-intent acceptance procedure

After verified closed-gate install and traced full startup, run the existing memory-type shader control on types2/3/5 with legacy quiet ICD (v1), then in the same device session with the isolated v2 ICD hash6B589A8686DF6FFBB2BE447845222A3607E7E162B89923FA5976FCD9FD412754. Each type runs16positive rounds, stale-input expectedfailure, then16repeat rounds in Limited interactive tasks. Validate actual loader path, hashes, per-process exit codes, exact CPU/GPU hashes and pre/post fence/fault counters. Capture KMD allocation gem_flags/Cached and system-leaf coherent/noncoherent/snoop-mismatch counters. A changed cache policy does not establish actual PAT mappings or borrowed table aliases. Stop above85C; no restart between these controls.


### M271-M272: startup and paired cache-intent runtime acceptance

Candidate0.7.100.1 installs as oem74 and starts full WDDM with every CP checkpoint, SDMA and interrupts successful; persisted log confirms the steps. Initial292paging jobs complete. Boot15:39:09 remains unchanged; the session followed owner AC removal, so warm reentry and0799 cause remain open. Legacy/v2 ICDs each pass96positive shader rounds plus3stale controls on types2/3/5, exact loader witnesses and18expected native exits. FinalGFX204/204,paging16284/16284,zero timeouts/refusals/noTDR. V2 interval adds23058coherent system-leaf encoding attempts with zero snoop mismatches; repeats/backgroundOS included. This does not identify actual CPU PAT/borrowed alias types or prove eviction+shader. Both tasks removed, newICD isolated in cache-intent-v2; globalquietICD unchanged. See factsM271/M272 and linked evidence.


### Candidate07100 v2 inference control procedure

Continue the healthy M272 device session using isolated cache-intent-v2 ICD in a Limited interactive task. Run the eight-operation Vulkan probe three times, then stories15M96tokens and TinyLlama64tokens with original deterministic prompts/seed1/temp0/ngl99/t6. Require actual loader witness, full-layer offload, native exit0 and exact Linux reference stdout (CRLF normalized only). Capture pre/post hardware counters,1000MHz/VID116 and temperatures. No DWM/device/OS restart or global ICD replacement.


### Candidate07100 v2 benchmark procedure

After exact inference reference acceptance, run existing bench0798 protocol with isolated v2 ICD, same b9564 executable/model files,pp512/tg128,r3,t6,ngl99,1000MHz/VID116. Require native exits,actual loader witness,Vulkan backend/99layers/three samples per row and zero hardware errors. Compare recorded M255 Windows and E14 Linux baselines with settings/build caveats. A single session is not proof of performance causality.


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


### Planned shader-after-eviction acceptance

Hypothesis: the installed full-WDDM KMD preserves shader-visible input bytes after explicit eviction/restoration. Use an isolated diagnostic build of the cache-intent-v2 ICD with shader-eviction-icd.patch, unchanged global ICD and KMD. The probe waits idle, fills fresh input and unmaps only that allocation at rounds1/5/9. A one-shot diagnostic hook records residency1, Evict, residency2/3, MakeResident and its paging fence, then residency1. The shader executes before any CPU remapping of input. Two integer-hash dispatches compare every output word with the CPU oracle. Run a positive baseline, eviction-positive and stale-input negative control through a Limited interactive task, recording exact loader/hash/submit witnesses and temperature/STOP checks. Failure to witness departure from GPU residency is inconclusive. This does not establish physical relocation or legacy aperture callback coverage. No OS restart is part of the procedure.


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


### Planned candidate07101 acceptance

Candidate0.7.101.1 includes local M278-M287 graph/partial-page builders and bounded table physical-cache observation. Build/sign and packagecheck the exact package before transfer. Preserve current07100 logs, verify STOP/temperature/clock and close hardware gates before installing with pnputil, without requesting an OS restart. Verify installed hash/version and healthy display-only startup. Then one traced full-WDDM start using persistent CP checkpoints, followed by small Vulkan shader/inference controls only if readiness is observed. Capture any physical-cache query status/type without treating it as per-VA PAT proof. This also exercises warm device reentry; cold-start success does not substitute for it. If remote observation stalls, re-poll the same host handle and inspect independent SSH state; never restart a timed-out observation as another test.


### M288: candidate07101 installed; full startup unresolved

Candidate0.7.101.1 includes localM278-M287. Exact package passes25checks, installs as oem75 with closed hardware gates, verified hash/version, deviceOK and unchanged Windows boot. One subsequent traced full-WDDM start lost SSH observation; its original host process remains live and independent reads time out. No last-stage evidence is available yet. Do not infer a CP/cache/alias cause or retry the startup. Owner monitor observation and read-only persisted-log recovery are pending. Warm reentry/full M9 remain open. See facts M288 and candidate07101-start evidence.


Recovery update for M288: the owner reports a desktop after disconnecting AC power. Remote recovery still times out; independent probes of both configured network routes do not reach TCP22. Local SSH service inspection is requested. No new hardware startup or reset was issued. Persisted checkpoints and installed runtime state remain unverified; cold recovery is not warm-reentry acceptance. See candidate07101-cold-recovery evidence linked in facts M288.


### Planned shared physical identity normalization (host only)

Hypothesis: equal-offset partial copy bands can share one sorted physical-page union per builder callback without changing copied bytes, multipass scratch ownership or published command coverage. Normalize all captured endpoints once, then pass each band's logical edge subspan to the existing planner. Validate with the actual KMD routing packet byte oracle, including partial ranges and constrained DMA buffers, and a WDK build. Compare old/new normalization plus planning on identical large partial-band input in a CPU-only harness; report host timings separately from GPU performance. No lab mutation is part of this experiment.


### M289: shared graph identity normalization (host only)

The equal-offset partial-copy builder normalizes the complete captured physical-page union once per callback and selects each band's edge subspan for validation/emission. This removes up to five repeated sorts without retaining state across OS calls. Actual KMD packet tests pass15684checks and the WDK build passes. A262144page host planning benchmark has matching semantic move digests and median187ms before versus48ms after (three trials); this is not a GPU/inference gain. Allocation remains128bytes per page and metadata is rebuilt per callback. No deployment while SSH remains unavailable. Full M9 runtime, cache, lifetime, general aliases and warm-reentry acceptance remain open. See facts M289 and shared-graph-normalization evidence.


### Planned virtual alias ordering regression

Hypothesis: current per-page virtual Transfer packet order overwrites a source needed later when distinct VAs describe a physical three-page cycle. The opt-in host test `paging_packets.exe --virtual-alias-ordering` supplies known synthetic VA translations, invokes actual WddmBuildVirtualTransfer and decodes/replays actual SDMA mapped-copy packets against an initial byte snapshot. A disjoint source/destination control uses the identical method. Expected current result: disjoint control passes, cycle byte oracle fails. This isolates construction order; it is not a real OS translation or GPU execution test. Keep default regressions separate while the production correction is pending.


### M290: virtual cross-page aliases fail the byte oracle

An explicit host regression now reproduces the unresolved virtual alias gap: source[A,B,C] to destination[B,C,A] through distinct VAs returns builder success but corrupts initial source bytes when actual SDMA packets execute in their emitted order. Disjoint copies pass the same translator and decoder. The opt-in `paging_packets.exe --virtual-alias-ordering` has32checks/1failure (native exit1); default15684regressions pass. This is a synthetic translation/actual packet construction test, not hardware evidence. Production correction is pending: capture and schedule dependencies for the whole transfer before publishing a prefix, while preserving table-shadow commits for local/table destinations. Reusing physical graph publication without those commits is insufficient. See facts M290 and virtual-alias-ordering evidence.


### M291: virtual system-page graph integration (local)

Equal-offset system/system virtual transfers now capture PFNs through the paging root and share the physical graph/band scheduler before any prefix publication. Tagged resumes and per-buffer scratch-complete groups handle bounded DMA capacity. M290's byte corruption is corrected for this path:15854default checks and170focused checks pass, including partial bytes and overwritten scratch between submissions. The broken-restore control fails10focused checks. WDK build passes; no deployment. Local/mixed endpoints retain the table-shadow-aware path, whose general cross-page aliases still require a correction; unequal offsets and actual OS/GPU lifetime/coherency/reentry acceptance remain open. Capturing and sorting all virtual system transfers also adds metadata cost that must be profiled and optimized. See facts M291 and virtual-system-graphs evidence.


### Planned graph acceptance through actual page-table walks

Extend the M291 byte oracle with actual VidMm CPU initialization and hierarchy walks. Reuse each disjoint/cycle/partial/multipass fixture in direct-initialized and queued-GPU-PTE-update modes. Before publication, the logical walker must see old pages; after accepted publication, graph construction must use the new system PFNs while the retained GPU-visible table image still has old entries. Replay actual copy commands against a byte snapshot. These remain host tests; actual GPU ordering and OS lifetime acceptance still require the lab.


### M292: graph acceptance through actual table walks (host)

The virtual graph byte oracle now includes real extracted VidMm hierarchy initialization and logical walks, as well as accepted GPU-PTE remaps whose retained GPU-visible table image is still old. Eighteen fixtures cover disjoint/cyclic/partial copies and two ring capacities across three translation modes. Default16348checks and focused664checks pass; omitting logical update publication causes17focused failures including byte corruption. This strengthens M291 construction-order evidence without claiming hardware execution or OS lifetime acceptance. Production source and installed driver are unchanged. General local/mixed aliases, unequal offsets, metadata costs, cache and recovery still require work. See facts M292 and virtual-graph-real-walk evidence.


### Planned logical table scratch foundation

Preserve known/unknown byte metadata alongside PTE bytes through a complete SAVE/COPY/RESTORE cycle. Use separate caller-owned logical scratch rather than registering a scratch page as a table. Verify full and partial three-page rotations against an independent original-slot snapshot with different unknown-byte patterns, and confirm a later unknown SAVE cannot reuse stale knowledge. This is a helper foundation only: actual graph publication, retained captured mappings across table-changing multipass calls and OS lifetime still need integration.


### M293: logical table scratch foundation

PagingPtShadowSaveBytes/RestoreBytes preserve both values and Known-byte metadata through complete copy cycles using a separate caller-owned scratch slot. Existing byte-copy semantics share the same inner loop. Host2434928checks (including124new full/partial cycle fixtures), kernel compilation and16348actual-routing checks pass; a generated broken restore fails118403checks. This helper is not wired into graph publication and is not hardware acceptance. Integration must reserve scratch outside the kernel stack, serialize accepted groups, and preserve captured physical plans across any multipass copy that changes its own translation tables. Re-resolving modified VAs is insufficient. MS local d3dkmddi.md4132/4144 guarantees progress-field preservation, not arbitrary retained-pointer lifetime. Local/mixed/table aliases and full M9 remain open. See facts M293 and table-shadow-scratch evidence.


### Planned graph-to-shadow batch composition

Connect the actual page-graph planner's complete batches to logical table SAVE/COPY/RESTORE operations. Preflight the entire batch before mutation; ordinary unregistered destinations do not acquire table ownership. Test eight-page cycles with full/minimal atomic budgets, local/system metadata and partial byte bands, compare final values/known bits to an initial snapshot, and overwrite logical scratch between submissions. This remains below WDDM publication: captured plan lifetime and hardware packet/metadata atomic acceptance must still be integrated.


### M294: planner-to-shadow batch composition (local)

PagingPtShadowApplyPageMoves now consumes actual planner identity indexes and complete batches, preflights them before mutation, and applies SAVE/COPY/RESTORE in emitted order. System sources are unknown metadata; ordinary unregistered destinations remain unregistered. Eight-page cycles pass independent snapshot checks with full/partial bands, local/mixed metadata and one/seven buffers despite scratch overwrite between batches. Host2672416checks and routing16348checks pass; a reversed-order mutation fails61361checks. WDK build passes, not deployed. This API is not yet called by WDDM publication: matching accepted GPU packets, preallocated scratch and stable captured plans across table-changing resumes remain required. See facts M294 and graph-shadow-batches evidence.


### Planned graph shadow publication integration

Add a callback-scoped graph descriptor and embedded logical scratch protected by VidMm's existing exclusive lock. The actual private/DMA publisher must validate capacity before applying the complete graph batch and publish the exact command bytes only after successful metadata commit. Test a real three-table cycle, generate direct-copy packets through the actual KMD builder, replay them independently, and compare every logical PTE while live backing remains unchanged. Short DMA/private capacity must preserve the original shadow snapshot. General graph builders still need to supply stable captured batches across multipass.


### M295: graph commit in actual paging publication

The actual private/DMA publisher now accepts an optional callback-scoped graph batch, validates buffer capacity before applying it, then publishes exact command bytes. VidMm owns embedded scratch and serializes the complete logical batch under its existing exclusive lock. A three-table cycle built with actual copy packets passes independent packet replay, all1536logical entries, refused-capacity atomicity and unchanged live-backing checks. Routing22513/focused6165checks pass; omitted graph commit fails1536checks. WDK build passes, no deployment. Existing production DDI builders still pass NULL for the graph: general local/table endpoint capture, matching emitted batches and retained plans across table-changing resumes remain required. See facts M295 and graph-shadow-publication evidence.


### Planned retained capture ownership

Keep captured CPU-only transfer metadata on the paging context, using non-repeating tagged resume identifiers rather than pointers. Completion detaches one capture; context or adapter teardown drains all remaining captures before freeing the object. Test interleaved plans, completed/stale identifiers and teardown ownership, then verify real WDDM teardown calls the drain outside its spin lock. This introduces ownership infrastructure only; graph builders must still attach captured plans and use their stable identities during resume. MS local d3dkmddi.md30891names the system-context handle;6290-6325defines context destruction and resource cleanup. These do not by themselves prove OS cancellation/reentrancy behavior.


### M296: capture ownership and context teardown

Contexts now contain a CPU-only capture owner with monotonic tagged identifiers, exact detach and drain operations. Tokens are not reused after completion or drain. Actual context destruction joins PagingBuildLock before object cleanup; object/adapter teardown drains remaining captures outside the spin lock. Owner46checks and actual extracted release64checks pass; WDK build passes, not deployed. Builders do not attach captures yet. Immutable endpoint capture, matching graph batches, completion detach and OS cancellation/concurrency remain required; this infrastructure is not general alias acceptance. Local MS d3dkmddi.md30891 and6290-6325 support the context handle and cleanup obligation, not a claim that arbitrary captured endpoint lifetimes are proven. See facts M296 and paging-capture-owner evidence.


### Planned immutable virtual graph capture

Capture all equal-offset source/destination identities and their system/local access domain into a single CPU allocation, normalize once and preflight every byte band. Derive atomic group capacity from the actual packet query and ring budget. Rebuild later graph batches from retained indexes without another VA translation. Test local, mixed and system cycles, full and partial ranges, after disabling translation and replacing the mapping fixture; scratch is overwritten between batches. This tests capture/planning, not emitted GPU execution or final DDI ownership integration.


### M297: immutable virtual graph capture

The new capture builder resolves equal-offset source/destination ranges once, retains normalized physical identities and local/system flags, and preflights every byte band. A resumed planner uses those immutable indexes and proposes the next cursor without advancing it before publication. Six system/local/mixed full/partial cycle fixtures pass after translation is disabled and mappings replaced, with scratch overwritten between batches. Routing22655checks and WDK build pass. Storage currently costs header+136bytes/page; it is not yet DDI-wired or deployed. Next work is captured-batch packet emission and exact graph publication, then owner attach/lookup/key validation/advance/detach. Unequal offsets, conflicting access-domain aliases and actual OS cancellation/lifetime remain open. See facts M297 and retained-graph-capture evidence.


### Planned retained graph packet emission

Emit one complete captured graph batch using physical-to-MC direct copies for local identities and bounded temporary GART mappings for system identities. Match the emitted actions to the descriptor returned for metadata publication, without advancing owner state. Extend M297's changed/unavailable-VA oracle to decode actual7/83DWORD packets and replay physical bytes, including mixed packets, partial offsets, exhausted DMA capacity and scratch overwrite between submissions. No DDI ownership integration or lab deployment is part of this host stage.


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


### M312 - Current acceptance review

[Current acceptance status](../../docs/research/m9-acceptance-status.md) maps the startup numbered requirements and full DMA scope to existing evidence and remaining gates. Source review confirms live paging capture allocation and propagated unsupported/resource errors remain unresolved. Both configured TCP22 routes failed again. No hardware action or driver change; recover07101breadcrumbs before another trial. See facts M312 and acceptance-review evidence.


### M313 - Actual07101startup evidence recovered

Owner-supplied changed target address restores pinned SSH access. Read-only acquisition and original file pulls recover successful GART/PSP/IH/GFX1..5 followed by CPstep1entry without persisted completion. Current FullWddm0/stage90/counter2/PnPfailed-post-start; no new hardware action. Prior old-endpoint failures do not prove sshd was down. Next inspect the CPstep1wrapper/KIQ sequence and guard state; exact hang cause remains unproven. See facts M313 and candidate07101-recovered evidence.


### Guard recovery after M313

See [procedure and hypothesis](../../evidence/windows/2026-09-23-E27-m9-inference/guard-display-recovery/PLAN.txt). One installed 0.7.101.1 display-only PnP restart after closing all hardware gates; no reboot or full GPU start. Result pending.

M314 result: read-only Stage90/counter2 verified; recovery execution rejected by automatic approval before launch. Explicit approval pending. No PnP restart or gate mutation; no recovery pass claimed. Source CP1 boundary review in the same evidence directory.


### M315 - Display-only recovery succeeds

After explicit owner approval, the prepared single closed-gate PnP restart succeeds. Device OK/CM_PROB_NONE, Stage50, info/log exits0 and unchanged Windows boot establish restored diagnostics. All hardware gates remain closed; no new deployment or full GPU start. This supersedes M314 pending approval, not the unresolved CP1 startup finding. See facts M315 and guard-display-recovery/approved-run.log.


### M316 - CP1 checkpoints prepared and replayed

Optional KIQ callbacks identify MQD/selection/register boundaries. Traced unpublished CP1 retains the same mutex and lock order using the documented critical-region ExAcquireFastMutexUnsafe pair, preserving PASSIVE_LEVEL for synchronous checkpoint files; normal paths are unchanged. Default and traced Linux replay match354+35writes, with10ordered cold-path checkpoints and all four negative controls discriminating. WDK build passes. No hardware deployment; persistence, lock/I/O behavior and hang location remain unverified. The development image also includes prior undeployed paging changes. See [M316 evidence and Microsoft contracts](../../evidence/windows/2026-09-23-E27-m9-inference/kiq-checkpoints/README.md).


### Candidate07102 startup trial

Hypothesis/procedure/expected outcomes recorded before execution in [candidate07102-start/PLAN.md](../../evidence/windows/2026-09-23-E27-m9-inference/candidate07102-start/PLAN.md). One traced startup after verified closed-gate installation, no Windows reboot.


Candidate07102 follow-up: repeat M283 shader oracle/eviction/stale control in the same GPU session. See shader-eviction07102/PLAN.txt and run.ps1; no reinitialization or reboot.


### M317-M318 - Current hardware startup and shader residency acceptance

Candidate0.7.102.1 installs and completes CP1..8 without an OS reboot. Durable CP1 checkpoints/KeepStatus0 establish that the diagnostic callback executes in this session; the active-queue recovery branch was not taken. Prior07101 cause and warm reentry remain unproved. Physical table cache queries return0xC0000141, not a valid cache-policy witness.

Same-session shader baseline16rounds and eviction16rounds match the CPU oracle; three positive1->2->1 residency cycles, with stale-input negative control failing as intended. Final graphics34/34 and paging15223/15223, no timeouts/refusals/TDR;1492virtual-transfer calls, individual dependency shapes not recorded. Initial script decoding errors were preserved and acceptance cross-checked from native files, installed identity and persisted logs. See facts M317-M318. General alias/cache/lifetime/resource-status/performance acceptance is still open.


### Current07102 residency scale

Repeat M25764KiB/1GiB positive residency oracle in the existing GPU session. [Procedure and acceptance](../../evidence/windows/2026-09-23-E27-m9-inference/gpu-residency07102/PLAN.md) recorded before execution. No reinitialization/reboot.


### M319 - Residency scale result on07102

64KiB control passes3cycles and4GPU byte-oracle readbacks.1GiB trial reaches the probe300s watchdog after2complete matching readbacks; full acceptance fails. Finalgraphics2495/2495,paging191078/191078,zero driver timeouts/refusals/TDR; sameboot and deviceOK. Changed stdout transport is an unproved timing hypothesis; native-file harness matching M257 is prepared with unchanged deadlines, not yet run. See facts M319. Preserve the working GPU session.


Native-file comparison after M319: [pre-run hypothesis](../../evidence/windows/2026-09-23-E27-m9-inference/gpu-residency07102-native/PLAN.md). Same deadlines/probe/driver,64KiB then1GiB, no reinitialization.


Conditional follow-up to native-file residency acceptance: repeat M274 inference baseline on07102, same settings/ICD/session; see bench07102-v2/PLAN.md. No reinitialization.


### M320-M321 - Large residency acceptance and current inference baseline

Restoring native-file output permits unchanged1GiB probe/300sdeadline to complete: all3NOTRESIDENT->GPU cycles and4fullword-oracle readbacks pass,4096GFXjobs. This establishes the observed large residency/content path on07102; arbitrary dependencies, PFN relocation and alias cache remain separate. Same-session TinyLlama1082.32/115.06 tokens/s remains below Linux1119.59/154.92 with existing configuration caveats. Finalgraphics16960/16960,paging425844/425844,zero timeouts/refusals/noTDR. NoGPUreinit/Windowsreboot or globalICD change. See facts M320-M321.


### M322 - Capture storage separated from allocation

Exact sizing and caller-owned in-place capture are implemented. Existing DDI behavior remains through a transitional allocating wrapper, so the resource-status gap is not closed. Actual-source328789checks include poisoned reuse with allocation unavailable, then existing packet/shadow oracles; WDK build passes. Microsoft system-paging1GiB geometry provides a reservation bound, but per-context concurrent-capture ownership must be established before integration. See facts M322 and capture-inplace evidence. No lab deployment or reset.


### Context capture reservation - procedure

Hypothesis: a SystemContext reservation sized for the documented 1 GiB system
paging VA window lets the actual captured-transfer builder complete and reuse
storage with dynamic allocation unavailable. Context drain must release an
active reservation once. Other pending captures retain their independent tokens.
Run actual-source KmdRouting fixtures with allocator failure during reserved
capture and all resumes, byte/packet oracles, second-operation reuse and drain;
then build with WDK 10.0.26100. No lab deployment in this step. Failure of any
oracle rejects the change. The transitional busy/oversized fallback still
allocates and does not close the complete paging resource/status requirement.


### M323 - System-context reservation integrated locally

SystemContext admission now reserves nonpaged21MiB plus capture header. The actual
captured-transfer DDI uses idle storage, retains it across callbacks and reuses it
on completion; context/stop drain releases active heap captures and reservation
once. Actual-source329129checks and WDK26100build pass. Interleaved busy-reservation
or oversized requests retain the allocating fallback, so the full resource/status
gate remains open. Host fixtures do not establish Windows context/stop concurrency.
See facts M323 and capture-reservation/RESULT.md. Development only, lab07102 unchanged.



### Prepared context admission - procedure

Test the actual context admission helpers with a list-publication observer:
system context must own its reservation when first visible, normal contexts
must not reserve it. Reservation allocation, object allocation and stop admission
refusals must leave no published object and no orphan allocation. Run the existing
routing/byte oracles plus these ownership checks and a WDK26100 build. This host
model does not establish Windows concurrent callback scheduling. No lab action.


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



### M327 - Scheduler access checkpoints, procedure

M326 last persisted progress is CP1 scheduler, before kiq_setting. Add callbacks
before the original RLC_CP_SCHEDULERS read, before its write and after the write.
No new MMIO or altered register sequence. Run traced and default Linux replay
oracles and build WDK26100. This may narrow a subsequent hardware observation;
it does not prove the prior hang was an MMIO access rather than checkpoint I/O.
Source reference: local Linux gfx_v10_0.c default kiq_setting, imported AMD MIT.
No hardware trial is implied by host acceptance.


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



### M344 - Isolate GFX retirement unbind and TLB boundaries (planned)

Hypothesis: the M343 RLC_BUSY transition occurs during a GTT unbind or one
of its VMID0 hub invalidations, before physical memory release. Add read-only
RLC_CNTL/GRBM_STATUS2 observations scoped to real GFX retirement only, with
allocation index/size and boundaries before/after unbind, after GFXHUB and
after MMHUB flush. Also observe before TearDown and before GpuMemRelease.
No register writes, ordering, timeout or quiet predicate are changed.
Build candidate108 with WDK26100 and run coordinator/stop-chain checks.
Before any hardware trial, verify exact package, preserve current107 evidence,
use a clean first-start control, run the unchanged64KiB GPU probe and stop once.
If the hypothesis holds, the first busy observation lies in a named unbind/flush
interval. If already busy before unbind, or only later, reject that interval.
Timing and diagnostic reads remain confounders; no causal/reset claim follows
from temporal bracketing alone. Current107 is display-only; do not warm-start
its already retired GPU as a substitute for the clean control.


### M344 - First retirement GFXHUB flush interval

108first-start/64KiBcontrol pass. During stop, CNTL0/STATUS2 0x8 remains after
first GTT unbind; after GFXHUB flush STATUS2 becomes0x01000008 and remains set.
Physical memory release, PSP unload and GART restoration follow. All reads0.
Successful after-PSP startup also has busy with CNTL1: busy alone is not a hang
predicate. Inspect AMD RLC_NO_KIQ wrappers/GFXOFF/KIQ ordering next; do not skip
invalidation to clear a symptom. Current108display-only,boot04:46:20. See M344.


### M346 - Extract GFX hardware halt from storage destruction

Before changing PnP ordering, split current Fini into hardware-only HaltEngines
and ReleaseStoppedStorage. Keep Fini composing them in the existing order for
manual diagnostics and current callers; no hardware behavior change yet.
Preserve the distinction between halt-register verdict and full undo/sequence
success. Retain every resource across the isolated halt phase. Validate actual
extracted source with the existing stop-chain matrix, historical negative
control, and an ownership witness between the two phases, then build WDK26100.
No deployment or warm-start acceptance follows from this preparatory extraction.
Both PnP stop and startup failure unwind must later use the complete phase split.


### M346 - Internal halt/storage extraction, not yet PnP integration

HaltEngines now preserves GFX ownership/storage; ReleaseStoppedStorage owns
the existing destruction path. Fini composes both in the original order.
1700actual-source checks and WDKbuild pass; no deployment. Both PnP stop and
startup failure unwind still need the full phase separation. GartStop cannot
simply move earlier: it destroys the adev/table owner that GFX teardown needs.
Add a distinct GART hardware-retirement operation while retaining that owner.
See facts M346; warm reentry remains unresolved.


### M347 - Integrate hardware retirement before GFX storage destruction

Use the same stop sequence for PnP and startup unwind: IH stop, GFX hardware
halt with ownership retained, PSP retirement, imported GART hardware disable,
GFX storage destruction, firmware GART restore and final owner destruction.
Keep diagnostic Fini composing its original helpers. Preparation flags latch
one hardware retirement attempt per device generation. A failed consumer or
translation retirement retains GFX/GART owners and backing rather than
performing destructive unbind. Preserve imported TLB invalidations; no RLC reset
is added. Test actual stop functions for ordering, retention, repeated stop,
partial state and successful release; build before any candidate deployment.
Hardware acceptance is a separate first-start/byte-control/stop/warm-start trial.


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


### M349 - Opt-in isolated RLC reload reset, source preparation

M348 changed retirement ordering but warm reload still leaves disabled/busy RLC
and stops logging at scheduler-read. Import gfx_v10_0_rlc_reset verbatim except
shim delay spelling. A diagnostic EnableRlcReloadReset gate defaults off. Before
PSP load in unpublished full startup, select only RLCdisabled/busy with all
CP/MEC/SDMA halt bits observed. Skip reset for busyclear or enabled RLC. Refuse
if selected but engines are not halted. Preserve unrelated reset bits and use
AMD's two50us waits, then observe busy; no new polling or reset of other blocks.
This state guard is a declared experiment, not an upstream ordinary-resume path.
Test no-write controls, halt requirements, two reset writes and postcondition;
replay unchanged ordinary init, build WDK, and keep hardware trial separate.


### M349 - Isolated RLC reset candidate prepared, not deployed

Candidate110 adds a default-off EnableRlcReloadReset experiment before PSP load.
Original AMD callback body comparison, ordinary354+35write replay,11reset-model
scenarios,327startup checks and25package checks pass. Model busy-clear is an
assumption, not hardware evidence. The gate requires disabled/busy RLC and all
CP/MEC/SDMA halt bits; post-reset busy or sequence fault refuses startup. No
resource-release or DMA-idle contract is weakened. See facts M349 and
rlc-reload-reset-preparation/RESULT.md for limits and hardware acceptance.
M348 remains the latest lab result; repeated startup is unresolved.


### M350 - Candidate110 hardware control and isolated warm-reset trial

Hypothesis: the imported isolated RLC reset, with measured halted-engine
preconditions, permits PSP/GFX reentry after the M348 retirement state.
Install exact110SYS with all execution gates and EnableRlcReloadReset closed.
Current recovered Windows boot05:20:01 has only display-only109 startup;
use that boot for the first full110control without a routine OS/AC reset.
First startup keeps the reset gate0 and must pass unchanged64KiB byte/residency
control. Preserve logs, close gates and stop; only the subsequent warm trial
sets EnableRlcReloadReset1. Capture RLC/engine preflight, reset status, PSP
results and scheduler checkpoints. If startup completes, require the same byte
control in a separate output directory. If rejected or hung, preserve logs and
recover using existing closed-gate/AC procedures as needed, with no unchanged
retry. Model success is not hardware acceptance. Record exact boot/hash and
telemetry independently of liveness. See M349 for source and validation.


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


### M352 - Explicit RLC reset readback variant, local preparation

Hypothesis: explicit post-write reads, before each original50us delay, change
observability or outcome relative to M350's isolated field-write callback.
Keep EnableRlcReloadReset0 off and1 the original callback; value2 selects a
separate sequence imported from the assertion/deassertion part of AMD's
GFXsoft-reset, narrowed to RLC only under the existing halt guard. Record
initial/asserted/deasserted reset reads after the sequence. Do not expand reset
domains, remove the busy postcondition, or log while reset is held asserted.
Test both variants against all existing guard scenarios and discriminate
register read/write ordering. Delay timing remains source/build evidence only.
Build candidate111; deployment and hardware acceptance remain a separate trial.


### M352 - Candidate111 explicit reset readbacks prepared

EnableRlcReloadReset2 selects AMD-derived post-write reads before both50us
waits;0off and1originalcallback remain. Existing halt guard/busy postcondition
remain, no broader reset domains.22model scenarios distinguish access ordering;
ordinary354+35replay,327startup and25package checks pass. Busy-clear is modeled,
not hardware evidence. Not deployed; M350 remains the lab result. See facts M352.


### M353 - Candidate111 same-boot readback trial

Use the M350 boot05:20:01, with110display-only and prior guarded reset refusal.
Install exact111SYS with all gates closed, then one full startup with
EnableRlcReloadReset2. This compares a changed sequence against the existing
same-boot M350 control and refusal; it is not a fresh111first-start control.
Observe reset initial/asserted/released values and RLCbusy, preserving logs.
If startup succeeds, require unchanged64KiB content control before workload.
If it refuses, preserve persistent logs then recover closed-gate display-only
through PnP. No unchanged retry or routine OS/AC restart. Readback agreement
proves register observation only, not reset-domain completion or DMA quiet.


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


### M355 - Scoped invalidate request/read/ACK observation, preparation

Add an optional callback around existing GMC invalidate operations: after REQ
write, after the existing GC10.1 dummy REQread, after the last ACKpoll sample.
The callback adds RLC observations only for GFXstorage retirement; normal paths
retain a null callback. Preserve original register operations and semaphore
release on timeout. Log raw sample plus final return/sequence status. Test
traced/untraced GFXHUB/MMHUB success and timeout with equal access histories;
callbacks must never acquire a semaphore or change hardware state. Build before
deployment. Observation timing may perturb hardware and must remain explicit.


### M355 - Retirement TLB observer candidate112 prepared

Scoped GFXHUB retirement callbacks expose existing request-write/dummy-read/
lastACK samples with RLCstate and sequence status. Ordinary wrapper uses null
callback; no invalidation or ownership policy changes. Eight flush scenarios
show equal original accesses with/without observer; ordinaryreplay,22reset,
327startup and25package checks pass. Not deployed. Hardware callback adds
latency/RLCreads and needs a first-start positive control before retirement.
See facts M355/tlb-observer-preparation; warm acceptance remains open.


### M356 - Candidate112 first-control and retirement substep witness

Install exact112SYS with all execution/reset gates0. Preserve existing M353
state evidence, gracefully shut down and perform one verified8sACoff/on for
first-start isolation after prior failed warm attempts. Gate0 reset remains
closed throughout. Require full startup and unchanged64KiB GPU residency/readback
control, then one closed-gate PnPstop with the new GFXHUBsubstep observer.
Collect request/dummyread/ACK values, sequence status and RLCstate; compare
first busy transition. This is an instrumented retirement test, not another
warm-reset test. Keep display-only afterwards and avoid an unchanged warm
retry. Preserve exact installed hash, boot, native output and power transitions.


### M356 - Invalidation ACK succeeds while stopped RLC becomes busy

112firstcontrol passes3cycles/4readbacks. During retirement, PTEunbind leaves
STATUS2 8; the first RLCsnapshot after request00F80001 sees01000008, before
dummyREQread. ACK1/flushrc0/sequencefault0 follow withbusyretained. Instrumented
interval includes observation/time; not proof of one faulting instruction.
See facts M356. Final112display-onlystage61/82presents,gates0,count0,newboot
06:06:14afteronebaselineAC. No warmretry. Review RLC-visible storage/firmware
lifetime and invalidation ordering before changing phases; do not skipflush.


### M357 - Separate CP retirement from RLC stop, local prerequisite

M356 motivates testing GTT invalidation after CP/SDMA halt but before RLCstop.
The current shim CSB is VRAM (AMD allows VRAM or GTT placement); GFX GTT pages are
ring/writeback/staging resources. First extract a keep-RLC variant of GFXfini,
leaving the existing fini contract unchanged. Model must show CPqueues halted,
RLC enable and CSB ownership retained, then explicit RLCstop before destruction.
Do not integrate into KMD until owner-scoped GTT unbind/flush can preserve all
backing storage until the later PSP/GART retirement. Invalidations cannot simply
be skipped. Existing failure results and diagnostic behavior must remain.


### M358 - Owner-scoped GTT mapping retirement, local prerequisite

Implement a storage-retaining operation for GTT entries owned by the active
sequence. Invalidate all selected PTEranges, flush both hubs once, then mark
translations retired only on success. Keep Bound as the historical exposure
flag, retain Used/Owner/Cpu/MC/Retired and all allocation backing. Later mem_free
must not repeat invalidation for successfully retired translations. Failure
retains mappings' ownership and dirty state. Test actual extracted functions
for mixed owners/domains, exact ranges, ordering, idempotence and failed flush.
No hardware integration/deployment until CP/RLC phase ordering is complete.


### M358 - Owner GTT mapping retirement helper ready for integration

New GpuMemRetireGttMappings unbinds current-owner GTT ranges,flushes bothhubs
once,then commits TranslationsRetired while preserving Bound/Used/Owner/backing.
Later mem_free avoids duplicate invalidation; GpuQuiet remains required for
release.38actual-source checks andWDKbuild pass. No stop caller/deployment yet;
next combine with M357CP/RLCphase split and validate ownership/order. See M358.


### M359 - Integrate owner GTT retirement before RLC stop

For completed CPstage, PnP/startup-unwind GfxPrepareStop now halts SDMA and CP
while retaining RLC, verifies existing halt/undo/sequence conditions, retires
current-owner GTTmappings with backing retained, then explicitly stops RLC.
Only successful mapping retirement contributes to GfxStopQuiet. PSP/GART and
storage phases follow unchanged. DiagnosticFini and pre-CP partial initialization
retain their existing paths. Test actual stop-phase source for success ordering,
retained owners, no mapping operation on failed halt, mapping failure retention,
then build candidate113. Hardware control/stop/warm trial remains separate.


### M359 - Candidate113 owner GTT retirement integrated before RLC stop

Completed-CP PnP/startup unwind now halts CP/SDMA retainingRLC, verifies halt,
unbinds current-owner GTT and flushes bothhubs with backing retained, then stops
RLC before PSP/GART retirement. Mappingfailure blocks quiet release. Diagnostic
and preCP paths retain existing behavior.3632stop/327startup/38GTT/25package
checks andWDKbuild pass. Not deployed; hardwarefirststart/content/stop/warm
acceptance remains. See facts M359/pre-rlc-gtt-integration/RESULT.md.


### M360 - Candidate113 hardware first-control, retirement and warm reentry

Install exact113SYS closed-gate. Existing112boot contains stopped/busy RLC;
use one normal shutdown and verified8sACoff/on for the first-control baseline.
Keep reset gate0. Require fullstartup and unchanged64KiB GPUcontent control,
then one closed-gate PnPstop. Preserve CPstop,owner GTTretirement,flush,RLCstop,
PSP/GART and storage witnesses. Attempt one same-boot full startup with reset0;
if successful require a separate-output GPUcontent control. On failure preserve
persistent logs before recovery; no unchanged retry or inference of faulting
instruction from last checkpoint. No claim of full M9acceptance from one trial.


### M360 - Retirement improvement confirmed narrowly; startup still fails

113firstcontrol passes.25ownerGTTretirements/flush occur withRLCenabled andbusy
clear; subsequentRLCstop/PSP/GART/storage/finalrestore keepCNTL0/STATUS2 8.
Warmstart again losesSSH,withbusy alreadyset beforePSP afterGARTinit;11PSP
commands success,lastCP1scheduler-read. MissingpreGARTsnapshot prevents exact
startupcause attribution. Next isolate startupGART invalidation andPSPmemory
visibility dependencies. Final113display-onlystage61/48presents,boot06:29:13,
gates0,count0afteronefailureAC. See factsM360;warmacceptance remainsopen.


### M361 - Startup GART observer and PSP dependency boundary

PSPring/command/fence/staging/TMR addresses in the current KMD are VRAMMC;
LOAD nevertheless requires GartEnabled and software completion includes it.
This is not proof of hardware independence from GFXHUB. Preserve ordering and
all invalidations. Add pre-GART RLCsnapshot and optional observedGARTenable:
after GFXHUBenable,afterMMHUBenable,afterfaultdefaults,afterMMHUBflush,and the
existing GFXHUBrequest/dummyread/ACKsubsteps. Use only automatic fullstartup;
legacy/PLAN wrappers remain ordinary. Compare source/replay/build before a
separate cold/warm hardware trial; no deferred-flush change is made yet.


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


### Candidate117 hardware trial

See [pre-execution plan](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07117/PLAN.md). First content control and successful SDMA quiescence are required before the single warm trial.


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


### Candidate118 reset lifecycle trial

Host integration M373 passed; see [hardware plan](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07118/PLAN.md). Require successful post-reset quiescence and inspect readbacks before one warm trial.


### M373-M374 - SDMA reset integration and hardware result

Candidate118 adds reset between two RLC scopes, then reestablishes post-reset
HALT/queue-off/cache-off before retirement. Host models and build pass. Hardware
first content control passes; both reset bits assert/release and post-quiescence
returns0/fault0. Warm still loses SSH after SDMA0-post RLCbusy; last persisted
point is entering RLC stage5, not CP1 in this trial. See M374. Final118 recovered
display-only, boot08:06:02, stage61/gates0/count0. Do not repeat unchanged118;
mask readback alone is not internal-reset proof. Warm and broader M9 remain open.


### M375 - Shared prepared storage for pending capture plans

Candidate119 assigns aligned arena spans to independent virtual-transfer plans
under PagingBuildLock, instead of letting one small plan monopolize the entire
context reservation. Completion/drain preserves tokens and frees the arena once.
Actual-route329178checks pass; old single-slot mutation fails24. WDK/25package
pass; not deployed. Exhausted/oversized heap fallback remains and no global
demand bound is claimed. See M375. Installed118/boot08:06:02 unchanged.


### Candidate119 arena hardware trial

See [pre-execution plan](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07119/PLAN.md). Sequential64KiB/1GiB and concurrent64MiB clients share one initialized session; no unchanged warm retry or routine stop.


### M376 - Candidate119 hardware content and post-workload incident

Installed119 passes64KiB and1GiB3-cycle/4-read content controls in one session.
FinalGFX4100/4100,paging170416/170416,noerrors/TDR;30reserved captures/0heap.
After successful script completion SSH becomes unavailable before collection,
without warm restart or device stop. Cause unknown. OneAC recovery returns
boot08:30:00; final119stage61/gates0/count0. Concurrent client trial not started.
See M376; content correctness does not establish post-workload session stability.


### Candidate119 concurrent/post-completion observation

See [M377 plan](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07119-concurrent-observed/PLAN.md). Fresh recovery boot, no1GiB retry, live summaries during two64MiB clients and60s afterwards.


### M377 - Concurrent clients and post-completion observations

Two11964MiB clients pass all byte oracles, with12reserved/0heap.60s polled
observation and a separate60s interval with only OS time heartbeats both pass;
finalGFX512/512,paging29899/29899,noerrors/TDR. M376 loss was not reproduced by
this changed workload; cause still unknown. Same-context arena overlap and
unbounded idle stability are not proved. Full119 session retained, boot08:30:00,
stage50/count0, GPU execution gates enabled; Full gate0 is one-shot, not display-only.
See M377. No reboot/AC/warm retry in this trial.


### Candidate119 shader eviction validation

See [M378 plan](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07119-shader/PLAN.md). Use retained initialized device, unchanged positive/eviction/stale-input probes, preserve exact native evidence.


### M378 - Current119 shader visibility validated

Retained initialized119 session passes16positive and16eviction shader rounds
with matching CPU hashes and3 residency departures/restores. Stale-input control
detects previous data as expected. Actual probe/ICD hashes and loader verified.
FinalGFX546/546,paging40813/40813,noerrors/TDR,32reserved/0heap. No device/OS
restart. See M378; cache attributes, PFN ownership and general aliases remain
open. Continue current-build inference/performance in this initialized session.


### Candidate119 inference validation

See [M379 plan](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07119-inference/PLAN.md). Retain current initialized session; exact text comparison and native GPU witnesses before performance measurements.


### M379 -119 inference content pass

Stories15M96/TinyLlama64 each match immutableLinuxGPU text with full offload;
M8eighttests pass. ActualICD/SYShashes verified; GFX2900/2900,paging73573/73573,
noerrors/TDR,76reserved/0heap. Oneprocess/model. Retainfull119boot08:30:00
for current performance measurement; broader acceptance remains open. See M379.


### Candidate119 benchmark

See [M380 plan](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07119-benchmark/PLAN.md). Current inference correctness passedM379; measurepp512/tg128,r3,t6 in retained session. Historical comparisons are not a fresh matchedLinux baseline.

### M381 - Capture demand and lifetime review

See [plan](../../evidence/windows/2026-09-24-E27-m9-recovery/capture-demand-review/PLAN.md). Inspect construction lifetime separately from GPU buffer lifetime before changing resource provisioning. No hardware transition.

### M382 - Capture pressure measurements

See [plan](../../evidence/windows/2026-09-24-E27-m9-recovery/capture-pressure/PLAN.md). Add construction occupancy and per-context peaks, validate extracted positive paths and build locally. No lab transition.

### M383 - Vulkan buffer alias visibility

See [plan](../../evidence/windows/2026-09-24-E27-m9-recovery/shader-buffer-alias119/PLAN.md). Reuse retained119 GPU session for two views of one intermediate allocation and an independent-memory negative control.

### M384 - Aliased allocation eviction

See [plan](../../evidence/windows/2026-09-24-E27-m9-recovery/shader-alias-eviction119/PLAN.md). Produce data before eviction, restore, then consume through the alias; retain119 session.

### M385 - Runtime-version reentry source comparison

See [plan](../../evidence/windows/2026-09-24-E27-m9-recovery/reentry-source-61852/PLAN.md). Acquire upstream6.18.52 and investigate distro provenance before choosing another warm-start change. No lab transition.

### M386 - SDMA timeout probe preparation

See [plan](../../evidence/windows/2026-09-24-E27-m9-recovery/sdma-reset-probe-preparation/PLAN.md). Source-derived memory poll with CPU release and execution marker; no hardware action in preparation.

### M387 - Exact Linux reset tracing

See [plan](../../evidence/windows/2026-09-24-E27-m9-recovery/sdma-reset-trace-preparation/PLAN.md). Isolated entry/return trace and runtime preflight; amdgpu_ring_reset is a macro. No hardware action in preparation.
