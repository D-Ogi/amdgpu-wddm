# UMD contract review for stage D (ADR 0008 stage D, ADR 0012, ADR 0013)

Read-only survey plus the decisions taken from it, 2026-09-22, against bc250kmd 0.7.24 and
`driver/contract/` as it stood before this review (caps blob version 2, submit blobs version 1).
Every claim carries a `file:line`; lines marked **DECISION** are this note's own choice, not
something measured. Style follows `docs/design/paging-node.md`.

## 0. Why this review, and what it found in one paragraph

`driver/contract/` was written before stage C's submission path (`wddm.c: CreateContext`,
`SubmitCommandVirtual`) and before ADR 0012 (many-in-flight, VMID-per-process) and ADR 0013 (two
nodes, SDMA is node 1) existed. Stage C and stage D (`docs/design/paging-node.md`) then built a
kernel side that **does not use the contract at all**: `grep`ing `driver/kmd/` for
`bc250_umd_private`, `bc250_umd_alloc_private`, `bc250_umd_context_private` or
`bc250_umd_submit_private` finds nothing. `DxgkDdiQueryAdapterInfo` refuses
`DXGKQAITYPE_UMDRIVERPRIVATE` outright (`wddm.c:1483-1488`, the `default:` arm's own comment names
it). `Bc250WddmCreateAllocation` parses a different, GDI-shaped private struct,
`BC250_WDDM_ALLOCATION_PRIVATE` (`wddm.c:138-146`), whose own comment already says what this note
confirms: *"This is our own private data, not the user-mode contract of ADR 0008 point 8
(driver/contract/): stage A has no user-mode driver to agree with, and the blob dies with this
stage."* `Bc250WddmCreateContext` sets `DmaBufferPrivateDataSize = 0` (`wddm.c:1601`), so a context
today is never handed a `bc250_umd_context_private` to fill in the first place, and
`Bc250WddmSubmitCommandVirtual` reads the raw DMA buffer at `DmaBufferVirtualAddress` directly
(`wddm.c:2047-2071`) - never a `bc250_umd_submit_private` blob.

None of that is a bug: it is stages A-C building the minimum spine (facts M71, M73, M77, M80)
without a user-mode driver to hand private data to. But it means the contract's job is not "catch
up with three new struct members" - it is "say, in writing, which of its 2026-09-21 assumptions
ADR 0012 and ADR 0013 have since settled differently, before RADV's winsys is written against
either version." The answer, in short: **one real architectural gap** (node 1 is not a submittable
queue family the way ADR 0013 point 3 assumed), a handful of confirmations that the existing
contract already has the right shape, and a long list of wiring that stage D's kernel code has not
done yet and this task does not attempt (it would mean writing the KMD's half of `CreateAllocation`/
`CreateContext`/`SubmitCommandVirtual` for a real UMD, which is M8's job, not this review's).

## 1. The gap list

### 1a. The node-1 queue-family gap (the one real architectural finding)

ADR 0013 point 3: *"The ICD exposes node 1 as its transfer queue family."* Measured against what
`docs/design/paging-node.md` actually built:

- Node 1's hardware path carries **only physical MC addresses and never reprograms a VMID**
  (`paging-node.md` section 2, **DECISION**: *"the paging node's SDMA submissions carry only
  physical addresses and never reprogram a VMID"*), reusing `VidMmTranslate()` against the *paging
  process's* root, not a submitting process's VMID-mapped address space.
- Node 1 is reachable **only** from `Bc250WddmSubmitCommand` (`wddm.c:2035-2041`, `hContext ==
  NULL`, VidMm's own paging queue) - never from `Bc250WddmSubmitCommandVirtual`, which is what a
  UMD's `D3DKMTSubmitCommand` on a GPU-VA context reaches. The guard is explicit
  (`wddm.c:2061-2065`, comment): *"node == BC250_WDDM_NODE_3D: this hardware path is GfxSubmitIb's
  ... node 1's own path (GfxSubmitPaging, SDMA0, no VMID) only exists behind
  Bc250WddmSubmitCommand ... A node-1 context reaching this DDI is unreached today ... this guard
  is what keeps it that way."*
- `Bc250WddmCreateContext` already **accepts** `NodeOrdinal == BC250_WDDM_NODE_COPY` once
  `wddm->NodeCount > 1` (`wddm.c:1581-1583`), which the `EnablePagingNode` gate can set today.

Chained together: a UMD that opened a context on node 1 and submitted through
`SubmitCommandVirtual` would hit the `node == BC250_WDDM_NODE_3D` guard, fall through to
`WddmCompleteSoftware` (`wddm.c:2069`), and have every fence reported complete **without the buffer
ever reaching the GPU** - silently, no error, no TDR. That is worse than refusing the context
outright, and it is exactly the shape of bug the contract's job is to make impossible to reach by
accident.

**DECISION, and the recommendation the task asked for:** M8's RADV winsys does not expose node 1 as
a queue family. `BC250_UMD_F_...` / the new `submittable_node_mask` (section 2) says so
explicitly, and README-winsys.md is corrected (section 4) to state RADV uses the gfx ring only,
matching what is already true on Linux for other reasons (fact M50: RADV drops `AMD_IP_COMPUTE`
outright for `FAMILY_NV`/GFX1013, and every dispatch already goes through the gfx ring). Reasons,
not just the mechanical guard above:

1. Building a VMID-addressed submit path for node 1 is real, undone work - ADR 0012's
   VMID-per-process/many-in-flight machinery (`gfx.c`'s ring allocator, the LRU VMID table, the
   `emit_vm_flush` sequence) exists for node 0 only; node 1 would need its own copy or a
   generalization, and `paging-node.md` section 5 explicitly chose **not** to generalize node 0
   and node 1's fence/channel state for exactly the reason that doing so would risk node 0's
   byte-for-byte regression bar. The same reasoning applies to the submit path itself.
2. `paging-node.md` section 8 point 2 already names the IB/VMID route as a deliberate, additive,
   not-yet-built upgrade with its own positive control (E24's H4) - this review's decision does not
   pick a new direction, it declines to get ahead of a decision `paging-node.md` already deferred.
3. SDMA is exercised and working today only as VidMm's paging engine (facts M95) and, before that,
   as a bring-up ring test. Nothing has measured an SDMA copy addressed through a VMID on this
   ASIC. Advertising node 1 to RADV would be exposing an unmeasured code path to a UMD that has no
   way to know it is unmeasured - hard rule 5 (positive control first) again.
4. Cost of waiting: none, because M50 already establishes the gfx ring covers the whole compute
   path Mesa 26.1.6 will actually run on GFX1013 - there is no RADV feature blocked on a transfer
   queue family existing in the first cut.

### 1b. Per-node fence: D3DKMT monitored fences vs the KMD's internal fence slots

Confirmed, not a gap: these are already two different, non-overlapping things and the contract
already models the user-visible one correctly. `bc250_umd_submit_private.fence_va`/`fence_value`
(`bc250_umd_submit.h:254-255`) is a GPU VA the IB itself writes - a D3DKMT monitored fence
(`D3DDDI_MONITORED_FENCE`, `README-winsys.md`'s synchronisation table) - and needs no KMD
involvement to signal (`README-winsys.md`: *"the signal side is free"*). The KMD's own fence slots
(`BC250_PAGING_FENCE_SLOT = 4`, `BC250_SUBMIT_FENCE_SLOT = 10`, `paging-node.md` section 5) are
internal bookkeeping on the SDMA0/gfx fence pages the hardware writes to - a UMD never sees a slot
number, only the monitored fence's own GPU VA and value. Worth writing down once, since the two are
easy to conflate: **section 4 below adds one paragraph to README-winsys.md and nothing to the
headers.**

### 1c. SDMA queue exposure to RADV in M8

Resolved by 1a: not exposed. See `submittable_node_mask` (section 2) and README-winsys.md (section
4).

### 1d. VMID/VA layout assumptions from ADR 0012 and M73

Confirmed, no gap: VMID assignment is per-process and entirely invisible to a UMD (ADR 0012 point
2, *"a context that keeps its VMID and root pays no flush at all"* - the flush is emitted by the
KMD on the ring, never by the UMD). The one VA-layout assumption that *is* UMD-visible was already
in the contract before this review: `BC250_UMD_A_EXACT_VA` (`bc250_umd_submit.h:64-69`) and the
64 KB reserve / 4 KB map distinction (`README-winsys.md` section "Address space"). Nothing in ADR
0012 changes it: VMIDs are switched under the hood by `gfx.c`'s ring code, and the page table
format and CPU-side walk (`VidMmTranslate`, facts M73) that both node 0's submission and node 1's
paging depend on are identical regardless of which VMID currently holds a given root. **No header
change.**

### 1e. Allocation private data for VRAM vs GART

Confirmed at the header level, real gap at the kernel level. `bc250_umd_alloc_private.preferred_heap`
(`bc250_umd_submit.h:89-91`) already carries `AMDGPU_GEM_DOMAIN_VRAM`/`_GTT` field-for-field from
RADV's own call (`radv_amdgpu_bo.c:566-592`) - nothing to add there. The real gap is that
`Bc250WddmCreateAllocation` (`wddm.c:1767-1823`) does not know this struct exists: it parses only
`BC250_WDDM_ALLOCATION_PRIVATE` (the GDI-surface struct, magic `"LB7A"`, `wddm.c:137`) and refuses
anything else (`wddm.c:1782-1790`, *"An unknown blob is an honest failure"*). The two private
structs have **different magics already** (`BC250_WDDM_ALLOCATION_PRIVATE_MAGIC` = `"LB7A"` vs
`BC250_UMD_ALLOC_MAGIC` = `"BC2A"`, `bc250_umd_submit.h:45`), so `CreateAllocation` dispatching on
`private->Magic` to accept either shape - the CDD's GDI surfaces and RADV's allocations, side by
side - is mechanically straightforward and does not collide with anything. **Left as an open
question (section 3) rather than implemented here**: doing it properly also means teaching
`DxgkDdiCreateAllocation` to honour `requested_va`/`BC250_UMD_A_EXACT_VA` against VidMm's
reservation (`README-winsys.md` open question 3, unmeasured), which is real new surface, not a
mechanical dispatch - M8 work, not this review's.

### 1f. The `ac_gpu_info` fields: from the blob vs from D3DKMT queries

Already fully enumerated and correct; this review re-verified it rather than finding a gap.
`README-winsys.md`'s "Device and adapter" table already separates the two: the caps blob answers
the one-time `ac_query_gpu_info()` set (`README.md`'s whole subject), while
`ac_drm_query_heap_info` (live, `QueryVideoMemoryInfo`), `ac_drm_query_info(TIMESTAMP)`
(`DxgkDdiCalibrateGpuClock`) and the residency/eviction counters (`QueryStatistics` or an escape)
are D3DKMT queries the blob deliberately does not carry, because they move at runtime and the blob
is fixed at adapter open. No change.

## 2. Header changes made (concrete, applied)

Both are additive, version-bumped, and were run through the kernel-mode compile probe and the host
test before being called done (section 5).

### `bc250_umd_private.h`: version 2 -> **3** (`BC250_UMD_PRIVATE_VERSION`, `bc250_umd_private.h:173`)

One field appended strictly after version 2's `reserved_v2[6]` tail, padded back out to a multiple
of 64 bytes exactly as version 2 did for version 1:

```c
/* --- appended in version 3, strictly after version 2's last byte --- */
__u32 submittable_node_mask;   /* bit N: node N accepts a UMD/VMID-addressed SubmitCommandVirtual.
                                 * Bit 0 (node 0, 3D) always set. Bit 1 (node 1, SDMA0/COPY) is
                                 * clear until a VMID-addressed submit path for it exists - see
                                 * section 1a. Not a Linux/amdgpu value: BC-250-specific WDDM
                                 * policy, the same class of "the ioctl does not carry this" field
                                 * as tiling/firmware/kernel already in this blob. */
__u32 reserved_v3[15];
```

`BC250_UMD_PRIVATE_SIZE_V3 = 1472` (was 1408). `offsetof(..., submittable_node_mask) ==
BC250_UMD_PRIVATE_SIZE_V2` is asserted, the same pattern version 2 used against version 1
(`bc250_umd_private.h:330-335`). `bc250_caps_unitA.c` sets `b->submittable_node_mask = 0x1u` for
unit A (section 5).

### `bc250_umd_submit.h`: `bc250_umd_context_private` version 1 -> **2**

One field appended strictly after version 1's `reserved[7]` tail:

```c
/* --- appended in version 2, strictly after version 1's last byte --- */
__u32 node_ordinal;   /* cross-check only - D3DKMTCreateContextVirtual's own NodeOrdinal argument
                        * is what actually selects the node (m7 section 3.5). The KMD refuses
                        * DxgkDdiCreateContext (one of the DDIs allowed to fail) if this disagrees
                        * with the DDI's own NodeOrdinal, catching a winsys bug where ip_type
                        * (AMDGPU_HW_IP_GFX/_DMA) and the node actually opened have drifted apart. */
__u32 reserved_v2[3];
```

`BC250_UMD_CONTEXT_SIZE_V2 = 80` (was 64). `bc250_umd_alloc_private` and `bc250_umd_submit_private`
are unchanged - section 1 found no gap in either that a header field would close; the submission
blob's existing `ip_type` field already carries what `SubmitCommandVirtual` would need to validate
per open question 8, and adding a second, redundant node-naming field to every submission (rather
than once per context) would cost more than it catches.

## 3. What the host test must check after the change, and what it already does

`driver/contract/test/run.ps1` needed no logic changes: `bc250_kernel_blob_layout()`
(`bc250_caps_unitA.c:621-658`) is used only for the Mesa-consumed fields, and `submittable_node_mask`/
`node_ordinal` are consumed by nobody in `ac_gpu_info.c` by construction (they are BC-250 policy,
not amdgpu UAPI), so they correctly do not appear there. What the run already re-verifies on every
call, unchanged:

- `bc250_contract_kernel_probe()` (`run.ps1:132-143`) instantiates all four structs and sums
  `.size + sizeof(...)`, so it would not compile if the new fields broke `/kernel /Zp8` packing or
  any `BC250_CTASSERT`/`BC250_SUBMIT_CTASSERT`. It compiled clean (section 5).
- The 165-check Mesa derivation and the 177-field device comparison
  (`compare_radv_info.py`) do not read the new fields at all, so an unchanged result (165/0, 177/0)
  is exactly the expected outcome, not a blind spot - both were re-run and both are unchanged
  (section 5).
- Added: `bc250_caps_unitA.c` now sets `submittable_node_mask = 0x1u` with a comment pointing at
  this note, so the blob the test builds is never silently zero (which would read as "node 0 is not
  submittable either", a wrong and dangerous default for a v3 reader that does not know the field).

Not added, and why: a dedicated assertion that `submittable_node_mask & 0x1` is set, or that
`node_ordinal` round-trips through a fake context. Both would be testing a value this file itself
chose (there is no independent measurement to check it against, unlike the Linux-derived fields),
so the assertion would only restate the initializer next to it - the comment carries the same
information with less code. If `Bc250WddmCreateContext` starts validating `node_ordinal` (section
1e's future work touches the same function), that validation gets its own test at that time,
against real DDI call shapes, not against this host-only harness.

## 4. README updates

- `driver/contract/README-winsys.md`, "Scope" section: add that node 1 is not exposed as a queue
  family in M8 (cross-reference section 1a of this note and ADR 0013 point 3's deferral), and that
  RADV's own gfx-ring-only behaviour (fact M50) makes this free rather than a restriction RADV would
  notice.
- `driver/contract/README-winsys.md`, "Synchronisation" section: one paragraph distinguishing
  D3DKMT monitored fences (what `fence_va`/`fence_value` carry, user-mode visible) from the KMD's
  internal fence-page slots (`BC250_PAGING_FENCE_SLOT`/`BC250_SUBMIT_FENCE_SLOT`, never exposed) -
  section 1b.
- `driver/contract/README.md`, "Files" table: `bc250_umd_private.h` line updated to "version 3,
  1472 bytes".
- `driver/contract/README.md`, "Not done" section: add the three concrete kernel-side gaps this
  review found and did not close (1a's guard already exists and is correct; 1e's `CreateAllocation`
  dispatch; the fact that `CreateContext`/`SubmitCommandVirtual` do not read any contract struct
  yet at all), so the next session does not have to re-derive them by reading `wddm.c` again.

## 5. Test and build results

- `pwsh driver\contract\test\run.ps1 -Out P:\BC-250\scratch\contract-stage-d`: **165 checks, 0
  failures**; **177 matched, 0 mismatched** against `radv-info.txt`; kernel-mode compile probe
  (`/kernel /Zp8`, all four structs) compiled clean; blob reported as **1472 bytes** (was 1408).
  Unchanged from before this review except the size, exactly as expected for an additive,
  Mesa-invisible field.
- `driver\kmd\build.ps1 -Kits P:\BC-250\toolchain\nuget -Out P:\BC-250\scratch\build\bc250kmd-contract
  -UmdStub P:\BC-250\scratch\build\bc250umd-0724`: clean, `/W4 /WX`, 0 errors / 0 warnings, both
  packages signed, version 0.7.24.1 unchanged. Expected: `driver/kmd` does not include any
  `driver/contract` header (confirmed by grep before touching anything), so this is a pure
  regression check that editing the contract did not disturb the driver build.
- `python tools\packagecheck\packagecheck.py ... --expect-version 0.7.24`: **PASS, 0 errors, 0
  warnings, 21 checks passed**.
- `python tools\wddm_contract_check\check.py`: **34 OK / 4 n/a**, unchanged.

## 6. Open questions that need the lab, or need M8's actual winsys code

1. **Whether `submittable_node_mask` should ever gain bit 1.** Answered by building the VMID-
   addressed submit path for node 1 (`paging-node.md` section 8 point 2's deferred IB/VMID route)
   and giving it a positive control - `paging-node.md`'s own E24 H4. Until that experiment exists
   and passes, bit 1 stays clear. No lab time needed to keep the current answer; lab time needed
   only to change it.
2. **`Bc250WddmCreateAllocation`'s magic dispatch (section 1e).** Not a lab question - it is a code
   change (branch on `private->Magic`, GDI struct vs `bc250_umd_alloc_private`) that can be written
   and host-tested without hardware. Left undone here because it also drags in VA-window
   reservation against VidMm (README-winsys.md open question 3), which *is* unmeasured: "a map of
   what VidMm reserves in a fresh process before the ICD runs, compared against RADV's heap
   layout." Experiment: a D3DKMT probe tool (extend `tools/win/kmtprobe`) that reserves nothing and
   walks the address space right after `CreateDevice`, before any allocation - the same shape
   `kmtprobe` already used for stage B (fact M73).
3. **Whether `DxgkDdiCreateContext`'s future `node_ordinal` check should be enforced now that the
   field exists.** Not done in this pass: enforcing it means `CreateContext` reading
   `pCreateContext->PrivateDriverData` at all, which it does not today (`DmaBufferPrivateDataSize =
   0`, section 0). Wiring that up is the same M8 kernel work as 1e and is not "unambiguous" in the
   sense the task asked for - it is the first line of the UMD/KMD contract actually being read by
   the kernel, and deserves its own reviewed change once RADV's winsys exists to write the other
   end of it.
