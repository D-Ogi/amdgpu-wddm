# M9 DMA contract audit

Current acceptance: [M9 acceptance status](../research/m9-acceptance-status.md), reviewed through M462. Historical results below do not prove current candidate acceptance.

Scope: WDDM paging builder, physical/virtual submission, private UMD payload, completion,
and CPU Present packet path. This review identifies gaps; it is not full WDDM certification.
Source/host implementation evidence spans M167-M257. Installed0798 passes automatic full-WDDM startup, real OS paging, Vulkan and four reference-matching processes per model (M251/M253/M254). M255 records benchmarks; M257 verifies three1GiB eviction/restoration cycles with full GPU readback. These workload results do not prove every contract below; alias, cache, lifetime, recovery and broader GPU pressure coverage remain open. Historical local-only assessments retain their evidence IDs.

Reference: Microsoft enriched local d3dkmddi.md (WDK26100); original documentation
commit7515063cea4c9e98db6a92986c5b4ddb0463fd16. Links below are workspace-relative.

| Area | Microsoft contract | Current assessment and next action |
|---|---|---|
| Paging DmaSize | BuildPagingBuffer example uses CurrentPagingBufferSizeLeft and updates remaining size; pDmaBuffer advances past written bytes. | FIXED locally in0758: remove double subtraction of DmaBufferWriteOffset and decrement DmaSize. Actual old-code regression fails, new passes. Full-WDDM runtime validation pending. |
| System memory / fragmented ranges | BuildPagingBuffer moves data to/from system memory; ADL Pages need not be contiguous. | PARTIAL LOCAL FIX0759: fragmented local ranges now split into page slices (M168). LOCAL INTEGRATION0766: virtual system-page copy/fill now emits bounded GART map/transfer/cleanup transactions (M178), with direct local/local retained. Host routing/multipass passes; runtime OS page ownership and GPU coherency unverified. Review legacy physical ADL operations and remaining unsupported/error semantics; no hardware limitation established. |
| Multi-buffer transfers | MultipassOffset preserves progress; insufficient DMA buffer requests a new buffer, and partial buffers are independently submitted. | LOCAL FIX0759: partial packets published before insufficient-buffer return; cumulative data-byte offset resumes, host1GiB coverage has no omitted/duplicate pages (M168). Full-WDDM runtime validation pending. Buffer ownership and runtime validation still required. |
| Physical submission offsets | SubmitCommand DmaBufferSize is TOTAL length; StartOffset/EndOffset select the submitted portion. | LOCAL FIX0760: physical command/private offsets checked against distinct sizes; exact command range bound to this submission's OS-owned private records. Parser tests pass (M169); actual physical/virtual DDI delivery still needs lab validation. |
| Virtual/private UMD payload | UmdPrivateDataSize is copied UMD length within ContextInfo private-data capacity; address space must be restored before submission. | Aligned in inspected normal path: checks UMD length<=capacity; BC2S parser; selected root and flush before ring. M166 validates supported inference, not all malformed-packet cases. |
| Virtual submission errors | SubmitCommandVirtual explicitly allows STATUS_INVALID_PARAMETER for malformed DMA/private data; OS faults calling device and handles ordered fence retirement. SubmitCommand differs. | PARTIAL LOCAL FIX0762: malformed/unsupported BC2S UMD packets return INVALID_PARAMETER before dispatch; rejected fence bookkeeping waits for prior hardware and completion publication, without synthetic DMA_COMPLETED (M171). Host ordering and parser tests pass; actual OS device-fault behavior remains untested. Malformed virtual Present now rejects; non-UMD paging validation remains open. Valid ring-refused work now enters an outstanding fault state (0767), with hardware recovery still unimplemented. Physical SubmitCommand error policy is unchanged. |
| Completion / timeout | DMA_COMPLETED fence identifies completed DMA work. | PARTIAL LOCAL FIX0761: watchdogs leave hardware pending for OS TDR, block subsequent software retirement and accept real late fences (M170). LOCAL MITIGATION0767: nonempty/UMD ring-refused work no longer retires in software or permits boundary preemption; actual predecessor fences still complete. ResetFromTimeout now explicitly fails instead of claiming hardware halt, so Windows may bugcheck/restart (M179). Actual successful GPU recovery remains unimplemented. |
| Paging shadow ownership | Each submitted buffer must execute its associated commands. | LOCAL FIX0760: global shadow/identity lookup removed. Per-buffer nonpaged private records remain OS-owned; ring copies selected words synchronously after whole-range validation, without clamping. Independent pending-buffer and malformed-record tests pass (M169). LOCAL FIX0763: a device-level shared/exclusive lock now protects PASSIVE_LEVEL builders through pointer lookup and engine teardown (M172). LOCAL FIX0764: device admission/reference drain protects CPU fence readers and paging submission before engine teardown (M173). Host model only; OS delivery, WDDM-object/ISR lifetime and hardware DMA recovery still need validation/review. |
| OS TLB invalidation | FLUSH_TLB identifies root/process and affected VA range; zero/zero selects all addresses. | PARTIAL LOCAL FIX0769/M182: ready-path operation12 emits full VMID1 invalidation in per-buffer records ahead of SDMA completion. Current WDDM uses only VMID1 and invalidates each root assignment, so no mutable process binding is captured. Host builder/packet tests pass; bootstrap/not-ready/error fallthrough remains OPEN, as do runtime cross-engine ordering and PTE publication. |
| Page-table update publication | Paging-process CPU_VIRTUAL initialization must update immediately; other updates use advertised mode. Local directories prohibit advertising CPU_VIRTUAL. | PARTIAL LOCAL FIX0770/M184: post-bootstrap GPU_PHYSICAL updates emit SDMA PTE write/barrier packets with entry-based multipass. CPU_VIRTUAL stays immediate; pre-RUN CPU bootstrap is serialized and permanently closed before first RUN. Full512 entries split480+32. Host encoder/builder tests pass; CPU mapping errors now propagate internally and leave bootstrap progress unchanged (0771/M185); full-source snapshots prevent partial writes on invalid entries. Outer error fallthrough, inherited hardware state and actual GPU/OS ordering still require closure. |
| Present | Present DmaSize is INPUT, unlike BuildPagingBuffer in/out; pDmaBuffer output advances. | Do not apply paging decrement blindly to Present. Current full consumption is intentional to isolate one private CPU packet. Separate discrepancy: pAllocationInfo is documented reserved/ignored, yet CPU blit reads it based on earlier observations. Needs WDDM2-specific contract verification; no claim of conformance. |
| Transfer page-table fields | SourcePageTable/DestinationPageTable are GPU VAs valid only with LegacyBehaviors.SourcePageTableVaInTransfer. | Comment corrected in0758. This cap is zero; do not reinterpret these fields as unconditional physical roots. Paging process context is used. |

Local references:
- ../../../ref/ddi-display/d3dkmddi.md:4119 (paging operations),4243 (remaining size example),30481 (multipass).
- ../../../ref/ddi-display/d3dkmddi.md:38960 (non-contiguous ADL pages).
- ../../../ref/ddi-display/d3dkmddi.md:35998 (physical submission length/offsets),36210 (virtual submission fields).
- ../../../ref/windows-driver-docs-ddi/wdk-ddi-src/content/d3dkmddi/nc-d3dkmddi-dxgkddi_submitcommandvirtual.md:61 (error semantics).
- ../../../ref/ddi-display/d3dkmddi.md:33644 (Present input size),33654 (reserved allocation info),19058 (conditional page-table VA).

Priority: correct mapping/fragmented transfers and multipass together with buffer lifetime and
completion semantics before using a1GiB pressure test as acceptance. Instrumented performance
work is independent. Do not describe a CPU readback lifecycle pass as GPU paging coverage.

Additional M168 finding: command budget must include the SDMA live-ring maximum reservation, fence and padding; OS DMA-buffer capacity alone exceeds that limit. Local0759 enforces this across all operations accumulated in one buffer.

Additional M174 local0765 fix: WddmStop detaches Device->Wddm before flushing DPCs, because IH remains enabled until the subsequent IhStop. Old/new extracted ordering test fails/passes a late-DPC pointer capture. ISR itself uses device IH/DCN state, not WDDM. Full runtime stop/interrupt and non-DPC DDI lifetime validation remain open.

System-memory implementation work is tracked in [m9-system-paging.md](m9-system-paging.md). M175 adds a host-checked PTE-write packet constructor; it is not runtime-wired and does not close the system-page gap. GPU-ordered TLB invalidation, reserved-window ownership and map/copy/unmap integration remain required.

M176 adds a host-checked SDMA GFXHUB/VMID0 invalidation packet constructor. System-memory integration is still incomplete: PTE-write/transfer memory ordering, GART reservation and complete command-budget/lifecycle handling remain open. No runtime paging claim.

M177 composes map/barrier/invalidate/copy/barrier/unmap/barrier/invalidate into one host-checked packet transaction with complete capacity reservation. Runtime GART/scratch ownership, system translations, fill/multipass integration and hardware cache/ordering validation remain open.

M178 supersedes earlier notes that the packet helpers are unconnected: they are now in the virtual paging builder. This is local source integration only, not hardware paging acceptance. Candidate0766 remains undeployed.

M180 local0768 adds CP initialization substep diagnostics to narrow the run015 stage6 stall. Existing replay and build pass; no hardware rerun yet, no reset/recovery claim. Buffered guard logs may be lost during a hard hang.

M183 adds an ordered SDMA PTE-write/barrier constructor, not yet wired to UPDATE_PAGE_TABLE. Full512-entry updates exceed the live-ring reservation even before outer completion; integration must split entry progress across buffers and preserve immediate CPU_VIRTUAL initialization. See ordered-pte-packets evidence for source-derived497+15 entry split and remaining bootstrap/error gates.

M186 local0772 removes the per-call CPU PTE snapshot allocation. Embedded storage is exclusively locked through encoding/writing; stop joins active writers. Host concurrency test rejects a lock-disabled mutation. This eliminates one resource-failure source, not the unresolved physical mapping or OS-facing BuildPagingBuffer failure policy. Microsoft0x119 documentation describes parameter0x5 as faulting system/paging commands; it does not enumerate an exact bugcheck mapping for every possible builder return code.

M187 local0773 moves mapping resources for immediate CPU PTE updates and ordinary page walks into VidMmStart. One retained NC segment mapping is protected by shared readers/exclusive writers and drained at stop. Resource failure now propagates through WddmStart to StartDevice cleanup before WDDM state publication. This removes per-update/walk mapping failures; it does not close other builder error semantics or prove full PnP/cache/hardware lifecycle behavior.


M188 deploys candidate0773 to unit A and checks display-only startup: installed
image hash/revision, PnP status, first presents and unchanged OS boot all pass.
This supersedes the last-known installed0757 state, not the unvalidated status
of full WDDM, retained mapping or GPU paging. All execution gates remain closed.


M189 extends ownership testing to actual VidMmTranslate's shared-lock wrapper:
concurrent readers overlap, writers/stop wait, post-stop/high-IRQL calls refuse.
Both exclusive-lock and shared-lock removal mutations fail. No GPU ordering claim.

M190 identifies an unresolved cache-alias policy in the retained segment mapping.
VidMmStart maps the whole segment NC, while WDDM advertises CPU-visible allocations
with Cached=0. Microsoft specifies consistent cache behavior across aliases and
write-combined backing-store defaults. Actual local-resident alias types remain
unmeasured, so this is a source-policy gap, not a measured cache corruption.
Review all overlapping mappings and choose a consistent policy or isolate the
page-table storage before opening full WDDM/GPU VA. Changing only VidMmStart to
WC would leave other NC aliases and does not establish correctness. See evidence
cpu-reader-lifetime for pinned source, primary references and exact limitations.


M191 adds an explicit failing ordering probe (`paging_packets.exe
--queued-pte-ordering`). When an actual GPU PTE update is constructed before a
transfer resolver runs, but has not executed, the actual CPU walker still sees
the old leaf. Applying the encoded update payload fixes the next resolution;
immediate CPU_VIRTUAL control also works. Default6221 regressions still pass;
this separate8-check acceptance probe has1 failure. No Windows callback sequence
or runtime failure is claimed. Establish paging-process initialization versus
subsequent update ordering, and guarantee that physical addresses captured while
building transfers include all prerequisite remaps. GPU execution ordering alone
cannot fix a stale address already baked into a transfer packet.

Cache-policy architecture constraint: a separate page-table segment is supported
by PageTableSegmentId/PagingProcessPageTableSegmentId, but is not a one-field fix.
Current root validation, CPU update bounds, PTE encoding of directory addresses,
leaf address bounds and query descriptors all assume the same local segment.
Any separation must change these together and keep data/table physical ranges
disjoint. Segment0 system-memory tables have a4KiB limit (current tables fit),
but require a separate ownership/mapping/update-mode design. No layout change
has yet been implemented or advertised to Windows.


M192 resolves the contract question left by M191: Microsoft's
[GpuMmu examples](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/examples)
place scratch-area PTE updates, flush and transfer/fill construction before paging
buffer submission. The [paging-process description](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process)
distinguishes fixed initialized hierarchy from scratch mappings used later.
A live-table CPU walk cannot generally substitute for ordered GPU translation.

Added `paging_pt_shadow.c/h` as construction-state groundwork: caller-owned bounded
storage, hashed physical table identities and known-entry bits, no GPU writes or
allocation.544 checks and a full dev build pass. It is NOT runtime integrated and
does not make M191 pass. Reserve capacity at start; register only pinned paging
process tables; commit logical changes only with accepted command batches; preserve
multipass and private-buffer failure behavior; resolve from this ordered state.
Power/lifetime/copy-PTE coverage and M190 cache policy remain required.


M193/local0774 wires logical-state allocation and CPU initialization. Storage
capacity derives from the fixed1GiB paging hierarchy (515 tables,1031 hash slots),
with start failure/cleanup and stop drain. CPU_VIRTUAL registration uses the
borrowed pinned table mapping only to obtain its physical identity; no borrowed
pointer is retained. Preflight checks capacity before immediate writes. Already
registered tables reflect immediate bootstrap GPU_PHYSICAL updates; other process
tables are not registered.6229 route checks,557 core checks and concurrency controls
pass. Queued GPU updates and transfer resolution remain unconnected, so M191 is
still failing. Next integration must commit logical batches only after command
and private-buffer acceptance; do not update future state on refused batches.


M194/local0775 completes source integration of logical GPU updates and transfer
resolution. WddmPublishPagingRecord accepts DMA/private capacity before committing
the exact entry batch. VidMmCommitPagingUpdate snapshots validated source entries
under the existing exclusive lock, modifies registered paging-table logical state
only, and leaves GPU-visible PTEs untouched. PagingResolve uses the logical walker;
missing entries refuse rather than falling back to stale physical tables.

M191 now passes at host scope:539 focused checks,6768 combined checks. A deliberate
omitted commit yields515 failures. Tests exercise publication refusal, unchanged
old GPU memory, known/unknown roots and480+32 multipass visibility. Full build and
existing private/lifetime checks pass. Full DDI dispatch, actual OS construction
concurrency, copy-PTE operations, power/reset and M190 cache policy remain open.
The helper preserves the existing malformed-publication INVALID_PARAMETER path;
this remains outside the documented BuildPagingBuffer return contract and must be
resolved with other outer error semantics. Candidate0775 is not deployed.


M195 adds logical copy metadata groundwork only. `PagingPtShadowCopy` requires
resolved physical identities, preserves known zero versus unknown, and supports
snapshot semantics across same-table overlap. Missing source clears destination
knowledge; missing destination does not register it.15935 core checks and existing
6768 route checks pass. `COPY_PAGE_TABLE_ENTRIES` remains unimplemented in the DDI.
The Microsoft copy-range fields are GPU virtual addresses aligned to64KiB, not
physical identifiers; a real builder must resolve them, construct ordered GPU
copies, handle array/multipass capacity, then publish matching metadata. The helper's
overlap policy is not evidence of hardware SDMA overlap behavior. Dev build only;
cache policy, OS errors, lifecycle/recovery and hardware acceptance remain open.


M196 adds an all-or-nothing staged PTE-copy packet constructor: source to a
caller-reserved temporary page, barrier, temporary page to destination, barrier.
Both actual copies are disjoint, allowing snapshot semantics without relying on
native SDMA overlapping-copy behavior.34 emitted DWORDs require48 aligned space;
outer completion and accumulated-buffer budgets remain the caller's duty.
8493 host packet/routing checks and a full dev build pass. No staging resource,
DXGK virtual-range handling, DDI publication or hardware execution is wired yet.
The host data model does not validate GPU barrier/cache behavior.


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


M201 (candidate0778) fixes the M200 diagnostic selection path: successful POST
cache attributes are retained and used by Vram Access and framebuffer sampling.
The shared selector compares physical pages, preserves NC for disjoint ranges,
and refuses mixed-domain ranges instead of extending WC onto unrelated pages.
2066 extracted mapping/policy checks pass; always-NC mutation fails2048. Full
build/sign passes; not deployed. Mapping callers are compiled/source-reviewed,
not host-executed. Other Present/DCN mappings and M190 OS-owned alias policy remain
open; this is not hardware cache/coherency or black-screen acceptance.
Evidence: `post-cache-0778`.


M202 (candidate0779) routes six Present/DCN surface mappings through the shared
POST-aware VramMapCpuRange wrapper. It preserves read-only/read-write access and
rejects invalid or unresolved protection before calling the OS mapper.2072 host
checks pass; always-NC mutation fails2049; full build/sign passes. Call sites are
compiled/source-reviewed, not dynamically covered. No deployment. M190 VidMm
OS-owned alias policy, table/probe/private-pool mappings and hardware coherency
remain separate unresolved work. Evidence: `surface-cache-0779`.


M203 adds address groundwork for a dedicated local page-table segment. Optional
context fields distinguish its ID/base/extent from application VRAM for both
PDEs and leaf mappings; size0 preserves existing behavior.2048 address/unit/kind
cases and invalid-layout controls pass, along with8676 routing regressions and a
full dev build. Not configured or OS-advertised. Segment enumeration, root/table
bounds and walkers must change together before use. Isolation alone does not prove
borrowed CPU_VIRTUAL mapping cache attributes; M190 remains open. Evidence:
`table-segment-addresses`. Dev package retains0779 and must not be deployed.


M204 adds VidMmStartLayout with separate application/table extents. Retained CPU
mapping and directory walks cover table storage only; roots and GPU update
destinations require its ID, while leaf bounds accept either local segment.
8693 host checks and full dev build pass. WDDM still calls the identical-range
VidMmStart wrapper, so enumeration/runtime layout is unchanged. Next integration
must select table capacity, split advertised segments and switch startup together.
Borrowed CPU_VIRTUAL mapping cache types/registration bounds and OS application
aliases remain unresolved. Evidence: `vidmm-table-extent`; dev package not for use.


M205 (candidate0780) wires the isolated layout into QUERYSEGMENT4, all page-table
level descriptors and WddmStart. Application1/aperture2/table3 use one shared range
calculation; ordinary allocation masks exclude3. Table budget is1/32 usable VRAM,
64KiB rounded, minimum4MiB.8922 host checks include actual two-pass descriptor
functions and stride guards; full build/sign passes. CPU_VIRTUAL physical identity
must fit the table extent before writes/registration. Not deployed. This removes
application/table overlap from the local source layout, not unknown cache aliases
with OS-owned table/application mappings. Runtime placement, pressure and capacity
acceptance remain open. Evidence: `wddm-layout-0780`; M190 is not closed.


M206 (candidate0781) removes new NC aliases from optional IB diagnostics. ProbeIb
walks the retained table map under the shared lifetime lock and reads OS system
RAM through MmCopyMemory physical-copy mode. Exact status/byte count is required;
short/error reads clear the diagnostic result.8939 host checks and full build pass;
not deployed. This removes a concrete diagnostic mapping hazard but does not settle
M190 borrowed CPU_VIRTUAL cache attributes or application aliases. It also does not
provide a GPU-consistent snapshot or prove OS page lifetime. See `probe-copy-0781`.


M207 checks candidate OS mapping APIs before substitution. MapFrameBufferPointer
maps a save/restore section; MapPhysicalMemory uses the cache type selected at
object creation, without an inspected guarantee for independent borrowed VidMm
pointers. Both require newer interfaces than current WDDM2.0. Do not treat replacing
MmMapIoSpaceEx with either callback as M190 closure. See `cache-callback-contracts`.
No driver or lab change. A no-alias redesign must preserve every required paging,
Present and diagnostic function; simply dropping accesses is not M9 completion.


M208 adds a portable page-list address resolver with separate first-page and byte
progress, fragmented/contiguous lists and overflow/page-bound checks.20196 address
checks plus13 boundary controls pass; existing stream and8939 routing regressions
pass. Not DDI integrated. The existing Transfer arm uses MDLs, while ADL-based
MapApertureSegment2 is newer. TransferOffset does not apply to MDLs; SegmentAddress
already includes the segment base. ADL addresses may be IOMMU logical and cannot
be treated as CPU physical mappings. See `page-list-addresses` evidence.


M209 adds GfxPagingMdlAddress over the actual WDK MDL access macros, bounding each
slice by ByteOffset/ByteCount before page-list resolution.21 new cases cover partial
first/last pages, PFN indexing and overflow;8960 total host checks and full dev build
pass. It borrows the MDL without mapping/locking/freeing it and does not add the
segment-only TransferOffset. Not DDI integrated; OS input reachability and lifetime
remain unverified. See `mdl-addresses` evidence. Dev package retains0781, not for use.

### M210: portable 64-bit transfer progress

PagingStreamBuild64 now carries total/start/next as 64-bit byte counts, while
packet sizes and DMA capacity remain bounded unsigned values. PagingStreamBuild
retains the old interface through a width-safe wrapper. Host coverage above 4 GiB,
carry, refusal rollback and existing routing tests pass ([M210 evidence](../../evidence/windows/2026-09-23-E27-m9-inference/stream64/)).
This does not widen WDDM MultipassOffset or remove the virtual builder's size cap.
Physical Transfer still needs an explicit external progress encoding and integration.

### M211: physical endpoint construction

GfxPagingBuildPhysical holds the existing engine lifetime lock and constructs SDMA
packets from independent source/destination endpoints. PagingStreamBuildEndpoints64
keeps equal offsets in distinct MDLs separate; older single-resolver interfaces
remain wrappers. MDLs use FirstPage plus byte progress, local addresses are already
MC addresses. Per-slice MDL/endpoint/device bounds and GART/PFN representability are
checked; failure publishes no current-batch progress.40 new host checks,9000 total,
portable wide regressions and full WDK dev build pass. See `physical-stream` evidence.
WDDM still must select segment extents, apply TransferOffset only to local endpoints,
validate operation flags, encode UINT MultipassOffset and publish accepted packets.
This is construction groundwork, not physical Transfer/Fill DDI or hardware closure.

### M212: preflight before partial physical transfer

GfxPagingBuildPhysical now checks the whole requested byte interval before emitting
anything. Previously per-slice checking could discover a short MDL or VRAM overrun
only after an earlier valid batch was already accepted. The constant-time preflight
checks MDL FirstPage/ByteOffset/ByteCount or the local device VRAM extent, integer
overflow and the internal system-address tag. Small-buffer refusal canaries and
exact-end controls pass (11 new/9011 total checks), as does the full WDK dev build.
See `physical-preflight`. PFN representability remains checked per slice; OS MDL
validity/lifetime is still a contract. WDDM must additionally restrict local endpoints
to the selected advertised segment and integrate flags/progress/publication.

### M213: physical Fill DDI route, candidate0782

DXGK_OPERATION_FILL selects local segment1/3 bounds, builds physical packets and
publishes accepted DMA/private records. MultipassOffset counts page slices,
preserving partial first/last DWORD slices and >4GiB resume. SegmentAddress already
contains the MC base.64 new/9075 checks and full WDK build pass; outer DDI branch
is source/compile covered. Evidence: physical-fill-0782. Not deployed.

Remaining correctness gap: filling storage with existing logical table-shadow
entries must publish matching shadow changes before later construction walks them.
M213 publishes DMA/private commands but does not update that shadow. Review virtual
fills and physical transfers to table storage under the same rule. Aperture Fill,
physical Transfer and the restricted OS return/fault policy also remain unresolved.
Internal errors from this arm propagate rather than becoming empty success; that
alone does not establish compliance with the restricted DDI contract.

### M214: physical table Fill shadow ordering, candidate0783

Supersedes the physical table Fill shadow gap recorded at M213. Private-header
preflight precedes VidMmCommitPagingFill under exclusive CpuUpdateLock, followed by
DMA copy and progress publication. The physical interval is checked against the
retained table extent. PagingPtShadowFill scans registered slots without registering
ordinary storage or touching live GPU tables. Full covered PTEs become known; partial
known PTEs preserve untouched DWORDs; partial unknown PTEs remain conservatively
unknown even after separate half writes. No uninitialized entry reads are required.
70999 shadow/9082 routing checks pass; omitted-commit control fails596. Full0783
build passes, not deployed. Evidence: fill-shadow-0783. Virtual Fill and arbitrary
copies into table storage still need equivalent logical publication; OS construction
serialization, GPU coherency and hardware behavior remain unverified.

### M215: partial PTE knowledge, candidate0784

Supersedes M214's conservative limitation for two separate half writes. Known now
tracks each DWORD independently; Apply sets both bits, Read requires both, Copy
preserves partial state with overlap ordering, Fill accumulates the written halves.
Storage is initialized on the first partial write without marking the untouched
half known. Bitmap cost increases64 bytes per table slot.112471 shadow/9082 routing
checks and full WDK build pass; no deployment. Evidence: half-pte-0784. This improves
logical-state fidelity; it does not wire virtual Fill or validate GPU/OS ordering.

### M216: virtual Fill logical publication, candidate0785

The dedicated virtual Fill arm builds one page slice at a time, obtaining the exact
physical identity from GfxPagingBuildFillPage. Accepted tracked-table slices commit
logical contents before DMA publication and before the next translation. Local and
system/GART paths remain; system pages do not enter the local table shadow. Input
command offset is restored after accumulating multiple records.24 new/9106 checks
pass, omitted-commit control fails599, full0785 build passes. Evidence: virtual-fill-0785.
Modeled translation tests verify captured identity and table effects; a new actual
walker self-remapping test and OS concurrency validation remain needed. Virtual
byte progress still limits size to32-bit. No deployment; transfer writes into table
storage, physical Transfer/aperture operations and restricted DDI error policy remain
open alongside hardware cache/lifetime/recovery/performance acceptance.

### M217: actual-walker virtual Fill dependency

The additional M216 real-walker test is now present: CPU_VIRTUAL initialization
creates a four-level hierarchy where the first Fill slice targets its leaf table.
Accepted zero fill invalidates the next slice's mapping in the logical view; the
builder refuses further translation while retaining its first published packet.
Live retained tables stay unchanged until independent modeled packet execution.
21 new/9127 checks pass; omitted logical commit control fails602 (3 new failures).
Evidence: virtual-fill-real-walk. This closes the mocked-translation test gap for
this dependency, not OS construction concurrency, actual GPU ordering or recovery.
Production source and candidate0785 unchanged this turn.

### M218: wide Fill progress, candidate0786

PagingStreamTokenEncode/Decode count the union of source/destination page boundaries,
with terminal partial-slice support and whole-range UINT-capacity preflight. The
physical and virtual Fill adapters share it. This supersedes M216's virtual32-bit
byte-size cap: virtual progress now uses slice tokens, so updating this candidate
requires a fresh device session, not preserving in-flight old byte tokens.
12405 stream boundaries and wide/capacity controls pass;9129 routing checks and
full0786 build pass. Omitted-commit control fails602. Evidence: paging-token-0786.
Physical Transfer remains unintegrated; its different-alignment codec groundwork
is tested but not OS/GPU accepted. Virtual Transfer still has the older size cap.

### M219: physical Transfer argument preparation

WddmPreparePhysicalTransfer prepares endpoints and64-bit byte progress without side
effects. Shared GfxPagingEndpointValid validates complete intervals. TransferOffset
applies only to segment addresses already containing their MC base; MdlOffset is a
separate PFN-page index. Start/End/Idle flags do not reset resume progress. Swizzle,
Unswizzle and reserved flags refuse; no claim of swizzle support.43 new/9172 checks
and full WDK dev build pass. Evidence: transfer-prepare. The helper is not yet called
by the physical Transfer DDI. Publication, effects on tracked tables, aperture and
OS restricted-error semantics still need integration. No deployment or GPU acceptance.

### M220: byte-granular logical copy, candidate0787

The logical shadow now tracks knowledge per byte, retaining complete-PTE read rules
and DWORD fill behavior. PagingPtShadowCopyBytes models a bounded one-page source/
destination slice with snapshot/memmove ordering; unknown external sources clear
only affected byte knowledge. Existing full-PTE copy preserves partial-byte metadata.
784240 shadow/9172 routing checks and full WDK build pass. Evidence: byte-shadow-0787.
Storage cost increases384 bytes/table slot versus0786. This is groundwork for
Transfer publication, not its DDI integration. Metadata overlap ordering requires
matching GPU staging/order; ordinary SDMA COPY overlap has not been established here.
No deployment or closure of OS lifetime/cache/recovery/performance acceptance.

### M221: staged byte-copy packet groundwork

The private-page two-copy/two-barrier constructor now accepts arbitrary1..4096-byte
ranges. Existing PTE wrapper preserves its stricter alignment/count contract.2349
new/11521 packet/routing checks pass with decoded data execution versus independent
memmove, capacity atomicity and alias guards. Full WDK dev build passes. Evidence:
staged-bytes. This supports single-slice snapshot ordering in the packet model;
GPU barrier timing, stage retirement and cross-slice overlap direction remain open.
Physical Transfer still needs captured source/destination identities and publication.
No deployment or hardware acceptance.

### M222: physical copy slice identities

GfxPagingBuildCopyPage constructs one page slice under the engine lifetime lock and
returns the physical identities/system flags used by its packets. Refusal clears
metadata. Local overlap stages; disjoint local and GART remain. System physical
aliases refuse pending mapped staging, despite different GART window addresses.
16 new/11537 checks and full WDK dev build pass; evidence copy-slice. Not DDI wired.
Transfer publication, table commits, whole-transfer overlap and GPU/OS lifetime
validation remain unresolved. No deployment or hardware acceptance.

### M223: mapped staged copy, candidate0788

Supersedes M222's one-slice system-alias refusal. Optional staging_mc retains both
GART mappings through source-to-stage and stage-to-destination copies, synchronization
and cleanup. Four markers,100 emitted/112 reserved DWORDs for two PTEs. Capacity and
stage bounds/alias checks precede output.11661 checks and full WDK build pass,
including decoded same-PFN alias data model. Evidence: mapped-stage-0788. No deployment.
Hardware barriers/cache, stage retirement, OS lifetime, cross-slice overlap and
Transfer DDI publication remain unresolved.

### M224: physical Transfer DDI publication, candidate0789

Physical Transfer now publishes prepared per-slice packets and updates tracked table
byte metadata under the exclusive VidMm lock after private-header preflight. Captured
packet identities prevent retranslation. UINT token counts completed slices; local
overlap chooses whole-range direction and forces staged barriers even for individually
disjoint slices. Accepted prefixes survive buffer exhaustion or later failure.
11700 host checks and full WDK build pass; omitted commits fail605. Evidence:
transfer-publish-0789. Outer DDI branch is source/compile covered; no deployment.

Incomplete: different multi-page MDLs are rejected before any output because cross-page
alias cycles are not yet analyzed. Mixed system/local cross-page aliases also need
review. Unknown MDL bytes copied to tracked tables invalidate metadata; subsequent
knowledge recovery is not implemented. Aperture/swizzle, restricted DDI failure
semantics, virtual Transfer table effects, hardware barriers/cache, OS lifetime,
recovery and performance acceptance remain unresolved.


### M225: documented scope and remaining uncertainty

[Source review and snapshots](../../evidence/windows/2026-09-23-E27-m9-inference/transfer-contract-review/review.md)
at MS documentation revision7515063cea4c9e98db6a92986c5b4ddb0463fd16 distinguish
an explicit guarantee from an omitted case. VidMm never requests aperture Fill;
earlier references to missing aperture Fill are superseded. Required aperture
map/unmap operations remain open. The inspected pages do not establish a ban on
two MDL endpoints or a non-overlap guarantee, so M224's restriction remains an
unresolved coverage gap. Transfer request ordering and AllocationIsIdle do not
prove staging-resource retirement. No code change, deployment or hardware claim.
Next: virtual Transfer table effects and required aperture map/unmap behavior,
alongside outstanding error, cache and lifetime contracts.


### M226: captured identities for virtual copy slices

GfxPagingBuildVirtualCopyPage resolves both endpoints once under the engine
lifetime lock and shares physical-copy packet emission through PagingBuildCopyPageCore.
Captured physical identities/system flags appear only after successful packet
construction; local/system aliases use existing staging.11712 host checks and
full WDK development build pass. Evidence: virtual-copy-slice. New translation
controls are modeled, not an actual-walker dependency test or GPU proof.
Next integrate this helper with virtual Transfer logical table commits and wide
multipass publication. Whole-transfer aliases, restricted return semantics,
cache/lifetime/recovery and hardware/performance acceptance remain unresolved.


### M227: virtual Transfer publication, candidate0790

WddmBuildVirtualTransfer now uses captured source/destination identities, private
header preflight and table-byte commit before publication and next translation.
UINT slice tokens remove the old4GiB byte-progress cap. An early DDI arm counts
actual moved bytes; unreachable legacy virtual Fill/Transfer arms are removed.
11746 checks and full WDK build pass. Actual-walker self-table copy invalidates
the next mapping after one accepted slice, while live contents remain unchanged
until decoded host execution. Omitted logical commits fail608, including3 new
dependency assertions. Evidence: virtual-transfer-0790. Not deployed; a fresh
device session is required for the new virtual Transfer token format.

Per-slice staging does not solve arbitrary cross-page aliases. Unknown-source
table knowledge, required aperture operations, restricted error policy, hardware
cache/barriers, staging retirement, OS lifetime, recovery and performance remain
open. Propagating helper failures replaces inherited empty success for this arm,
but is not proof of conformance with the restricted BuildPagingBuffer returns.


### M228: reserve OS aperture geometry separately

Source review found QuerySegment4 still advertises base0, overlapping the numeric
GART range used by driver allocations and temporary paging views. Map/unmap is
not implemented; directly adding writes against that descriptor risks corrupting
private mappings. No runtime corruption is claimed.
PagingApertureInit/Range provide a bounded256MiB OS extent after both consumers.
11832 host checks and full WDK dev build pass; evidence aperture-partition.
These pure helpers are not yet connected to the descriptor or mapping operations.
Next capture actual geometry via GartDevice (lazy setup, lock/lifetime required)
and share it between QuerySegment4 and ordered map/unmap PTE construction.
Do not assume actual gart_start is zero. DummyPage, cache flags, OS lifetime,
invalidations and all hardware acceptance remain open.


### M229: captured aperture descriptor, candidate0791

GartCaptureAperture derives M228 geometry from existing GartDevice setup under
GartLock. WddmStart captures it before VidMm startup/WDDM publication; missing
resources propagate through StartDevice cleanup. QuerySegment4 uses cached MC
base, size and commit limit; missing geometry refuses. Full WDDM with local
segments therefore now requires GART setup resources; display-only is unchanged.
11839 checks and full build pass; evidence aperture-layout-0791. Capture/query
code is host executed with modeled setup/locks; startup integration is compiled
and source inspected only. Not deployed. A fresh device session is required.
Next map/unmap must use this geometry and validate it against live GART state.
CPU aperture mapping policy, cache flags, DummyPage, hardware ordering/lifetime,
error handling and acceptance remain open.


### M230: ordered permanent aperture packet constructor

bc250_sdma_paging_set_aperture composes explicit PTE writes, fence/poll and GART
VMID0 invalidation with one complete reservation (29+2*N DWORDs, aligned to16).
Encoded DummyPage entries are supplied explicitly; NULL cannot silently unmap
to zero. Marker/PTE overlap and address limits are checked before output.
13634 checks and full WDK dev build pass; evidence aperture-packets. Not DDI
integrated or deployed. Next connect OS MDL/cache/dummy semantics, live geometry
validation, bounded multipass and private publication. Packet agreement is not
hardware barrier, cache or OS-lifetime acceptance.


### M231: aperture map/unmap publication, candidate0792

Ready-engine map/unmap now uses live-validated captured geometry, MDL page
identities or repeated OS DummyPage PTEs, cached TT snoop selection and the M230
ordered transaction. Batches fit live-ring capacity and a256-entry stack array.
Private record acceptance precedes MultipassOffset advancement (page count).
13678 checks and full0792 build pass; evidence aperture-ddi-0792. Not deployed.

Pre-RUN startup is explicitly unresolved: PagingReady is set only after explicit
RUN stage8, so early OS mapping requests refuse. Advertisement/initialization and
mapping preservation must be reconciled before full-WDDM deployment. This ready
path and its propagated internal errors do not prove restricted DDI compliance.
Physical Transfer segment2, CPU aperture mapping policy, cache/barriers, OS page
lifetime, cross-engine ordering, GPU recovery and pressure/performance remain open.


### M232: startup readiness must precede operation admission

[Startup design and acceptance gates](../design/wddm-startup.md) record the source
ordering: manual full-WDDM activation precedes GART/PSP/IH/RUN, and ENABLE zeros the
full GART table. Early CPU mapping followed by that enable is therefore not a
complete startup solution. E16 run009 saw no map call; no early-map crash is claimed.
Existing kernel firmware loading can be reused. Next extract shared initialization
entry points and validate partial-failure ownership before automatic activation.
Moving RUN into StartDevice also changes failure-cleanup assumptions and requires
hardware halt/retention evidence. Source snapshots: startup-contract-review.
Candidate0792 unchanged; no new runtime validation.


### M233: preserve teardown dependencies after failed retirement, candidate0793

Fini and GfxStop carry halt/undo/sequence status into page release and a retained
stop verdict. PSP unload requires confirmed GFX stop; GART restore requires GFX,
IH and PSP retirement. Failed teardown retains owners/views/dummy memory and a
second stop cannot erase the verdict. Same-device-object restart refuses before
new initialization.1676 extracted-function checks pass versus889 failures in the
previous source; full WDK build passes. Evidence stop-chain-0793. Not deployed.

This is failure containment by resource retention, not successful GPU reset.
The latch is per device object; remove/re-add or driver reload containment,
OS-owned DMA lifetime and diagnostic escape retry policy remain unresolved.
Automatic startup still requires the rest of the M232 ownership/admission plan
and actual hardware stop evidence.


### M234: shared internal initialization entry points

Gart/Psp/Ih/GfxInitializeHardware now share the unchanged command implementation
with their diagnostic Escape entry points. Caller-owned nonpaged reports preserve
partial progress; wrong IRQL, missing input, quarantined device, command refusal,
shim error and incomplete completion refuse.141 host controls pass; omitted
completion checks fail30; full dev build passes. Evidence init-entrypoints.
No automatic activation or deployment. Next preallocate/preflight, coordinate
ownership and unwind, verify actual paging readiness, then connect WDDM admission.
Stage8 alone does not prove that paging resources/gates are ready.


### M235: firmware preflight and retained input ownership

PspPrepareFirmware validates existing file/layout inputs without hardware writes
and returns an opaque owner. PspInitializePrepared borrows the same file buffers;
PspReleaseFirmware explicitly releases them after success or unwind. The existing
diagnostic path retains its own read/free behavior.32 new controls,141 initializer
regressions and full dev build pass; evidence firmware-preflight. I/O, parsing and
hardware outcomes are modeled. Next integrate report allocation, preflight and
hardware phase ownership in the startup coordinator. No automatic startup or
deployment; readiness/publication and hardware acceptance remain open.

### M236 - startup coordinator (2026-09-23)

Added GpuStartupInitialize and GfxStartupResources. Firmware and aggregate report
storage precede hardware activation; partial attempts unwind IH/GFX/PSP/GART.
Final checks include the system-memory paging window and preserved aperture
geometry. Source/host controls:291 pass, two mutations fail11/3; stop1676 and
firmware32 regressions pass. See [evidence](../../evidence/windows/2026-09-23-E27-m9-inference/startup-coordinator/).
WDK development build passes; official0793 and last known installed0773 unchanged.
No lab access. Not wired into WddmStart: interrupt ordering/admission integration
and all outstanding live paging, lifetime, reset, error-policy and performance
acceptance remain open.

### M237 - interrupt admission preflight (2026-09-23)

Coordinator now requires actual SynchronizeExecution callback delivery before
hardware attempts, following local MS connection-failure semantics.319 host
checks pass; bypass mutation fails7; WDK dev build passes. [Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/interrupt-preflight/).
Source review found IH hardware enable precedes DPC context publication; fix the
publication/final-enable transition before automatic startup integration.
No deployment, no observed lost interrupt claim, all live acceptance gates open.

### M238 - split IH preparation/enable (2026-09-23)

Shim split preserves AMD register order:14 writes with delivery disabled, then
one final enable. E03 replay and existing IH controls pass; WDK dev build passes.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/ih-split/).
KMD publication/synchronization integration is still required; no lab deployment
or claim of corrected runtime interrupt delivery.

### M239 - synchronized IH activation (2026-09-23)

Real INIT publishes complete DPC state after hardware preparation and before
synchronized final enable. DIRQL uses owned report storage and restricted MMIO.
95 host checks pass; late-publication mutation fails7; regressions/build pass.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/ih-publication/).
Stop/failure lifetime review and startup admission integration remain open,
alongside all outstanding M9 hardware paging, reset and performance gates.

### M240 - IH stop admission and owner retention (2026-09-23)

Synchronized ISR admission closure precedes DPC flush. Failed synchronization or
halt retains IH owner/backing and sets sticky quarantine; failed GART lookup also
closes software admission.177 host checks pass; no-sync mutation fails54;
activation95/stop1676 regressions and WDK build pass.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/ih-stop-admission/).
No deployment. OS timing, actual retirement, remove/reload containment and full
M9 paging/performance acceptance remain open.

### M241 - WDDM admission integration (2026-09-23)

GPU startup now precedes WDDM state publication; missing required gates/layout
or startup failures prevent admission and clean unpublished VidMm resources.
FullWddm diagnostic mutations are refused.89 host checks and319 coordinator
regressions pass; omission mutations fail8/4; full WDK dev build passes.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/wddm-autostart/).
Not deployed. Manual initialization scripts need replacement. Correction M242: clock control
uses independent bc250rd SMU IOCTLs, unaffected by the miniport escape guard;
its ordering before GPU start still needs verification. OS callback timing, lifecycle, real paging and
remaining M9 correctness/performance requirements remain open.

### M242 - corrected clock path and readback verification

bc250rd clock IOCTLs are independent of miniport escapes. M241's contrary
assumption was corrected. CLI verification now compares frequency and encoded
VID;10 modeled scenarios pass, omitted comparison fails2. Fresh service/task
snapshot confirms connectivity, not current clocks or startup ordering.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/clock-path-review/).
Use the existing serialized mailbox owner, not a competing KMD writer.

### M243 - live clock verification control

Query-only CLI control passes on unit A:1000MHz/VID116, deliberate1001 expectation
refused,1000 recheck passes.67C before test; no clock-setting message or reboot.
[Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/clock-live-control/).
Next prepare a versioned automatic-start trial with one-shot gate containment
and startup-aware scripts. No new KMD or paging acceptance evidence yet.

### M244 - existing one-shot gate durability

Existing EnableFullWddm1 consumption now checks write/flush results and refuses
on failure; no duplicate gate added.40 host controls pass vs7 failures in prior
source; WDK dev build passes. [Evidence](../../evidence/windows/2026-09-23-E27-m9-inference/oneshot-durability/).
Per-load semantics do not imply per-StartDevice containment. No live deployment
or new paging/performance acceptance.


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


### 2026-09-23 CPU_VIRTUAL cache contract recheck

Checked the local UPDATEPAGETABLE Markdown (lines65/101), enriched d3dkmddi.md and current Microsoft Learn UPDATEPAGETABLE/System paging process descriptions. They specify a CPU virtual address and immediate CPU initialization, but the inspected sections do not specify UC/NC/WC/WB attributes for that pointer. This is a bounded documentation finding, not proof that no Microsoft document specifies it. Cached=0 describes default write-combined allocation backing store; it does not by itself establish the cache type of the borrowed page-table mapping. Keep M190 open. A shader visibility test also cannot establish CPU alias attributes.

Online sources checked2026-09-23:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_buildpagingbuffer_updatepagetable
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process

Prepared shader-coherency-probe.c separately to test positive shader visibility: unchanged E14 Vulkan setup/inthash shader, reused host-coherent input/intermediate/output allocations,16 rounds of changed inputs, two chained shader dispatches with existing barriers/fence, full CPU byte oracle and distinct sentinels. Optional --stale-input-control omits one input rewrite. Build/help pass under MSVC /W4 /WX /wd4702 (inherited E14 unreachable fallback return warning), but no GPU run or negative control has occurred yet. Procedure is in E27 README; no runtime coherency claim.


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



### M375 - Shared prepared storage for pending capture plans

Candidate119 assigns aligned arena spans to independent virtual-transfer plans
under PagingBuildLock, instead of letting one small plan monopolize the entire
context reservation. Completion/drain preserves tokens and frees the arena once.
Actual-route329178checks pass; old single-slot mutation fails24. WDK/25package
pass; not deployed. Exhausted/oversized heap fallback remains and no global
demand bound is claimed. See M375. Installed118/boot08:06:02 unchanged.


### M376 - Candidate119 hardware content and post-workload incident

Installed119 passes64KiB and1GiB3-cycle/4-read content controls in one session.
FinalGFX4100/4100,paging170416/170416,noerrors/TDR;30reserved captures/0heap.
After successful script completion SSH becomes unavailable before collection,
without warm restart or device stop. Cause unknown. OneAC recovery returns
boot08:30:00; final119stage61/gates0/count0. Concurrent client trial not started.
See M376; content correctness does not establish post-workload session stability.


### M378 - Current119 shader visibility validated

Retained initialized119 session passes16positive and16eviction shader rounds
with matching CPU hashes and3 residency departures/restores. Stale-input control
detects previous data as expected. Actual probe/ICD hashes and loader verified.
FinalGFX546/546,paging40813/40813,noerrors/TDR,32reserved/0heap. No device/OS
restart. See M378; cache attributes, PFN ownership and general aliases remain
open. Continue current-build inference/performance in this initialized session.


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
