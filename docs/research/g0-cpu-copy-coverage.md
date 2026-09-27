# G0 CPU-copy evidence coverage after M679

This is a source-bound review and a plan for closing measurement gaps, not a new
G0 acceptance claim. Runtime facts are M679 in docs/facts.md. Exact UMD source
is a0ad8af5ea46b90400d947d82d056f3bd58278c5, linked by the M597 build receipt to
UMD5C74BF98 used in M679. PROVENANCE: Mesa, MIT.

## What the current evidence covers

`classify-map-coverage.py` consumes the recorded Zink audit, checks map-call and
persistent-call totals against buckets in each snapshot, rejects overflow, and
reports runtime-resource, user-pointer and persistent-image mappings separately.
It does not interpret missing entries as evidence when totals fail reconciliation.
Run it against the private raw log or M679's public selected-audit.log. The latter
retains all audit lines and omits unrelated stderr. Source definitions are in
`zink_resource.c:2645` (bucket key) and `zink_context.c:101` (cumulative snapshots).
For M679 it reports154 complete snapshots with none of those three map classes.
The existing byte reconciliation remains a separate check; this tool counts calls.

The `runtime` marker comes from the imported object's `bc250_runtime`, set in
`zink_resource.c:1115` for an identity-bearing runtime import. It therefore covers
maps through these Zink entrypoints, not arbitrary accesses to a shared CPU pointer.

## Persistent-buffer interpretation

M679 records seven24,000-byte persistent buffers with bind0x8008000/usage0x703,
and one1MiB buffer with bind0x8070/usage0x322. These are signature matches, not
unique allocation provenance: the current bucket key omits resource identity.

- `zink_descriptors.c:1668` maps batch descriptor buffers with READ, WRITE,
  PERSISTENT, COHERENT and THREAD_SAFE (0x703), with ZINK_BIND_DESCRIPTOR.
  Its db_map users encode/copy descriptor records. This matches the first class.
- `u_upload_mgr.c:93` creates the default1MiB vertex/index/constant uploader;
  persistent flags at:77 produce0x322. `zink_context.c:5993` creates the stream
  uploader and:5994 the constant uploader. Zink buffer DISCARD staging also uses
  an uploader (`zink_resource.c:2760`), so calling this only constant data would
  overstate the source evidence.
- `u_upload_alloc` advances offset monotonically. Exhaustion calls
  `u_upload_alloc_buffer`, which creates and maps a new resource. There is no
  automatic wrap into the previous persistent buffer. Under this allocator's
  normal ownership contract, one1MiB map cannot service repeated new full-frame
  payload allocations without further maps. This is a source inference; it does
  not measure writes through retained pointers or establish this bucket's caller.

## Present path boundary

In `DxgiFns.cpp`, the hosted branch of Bc250EnsureSurface imports the runtime
allocation and returns before the CPU Lock2/user-memory branch. Hosted _Present
flushes GPU work, calls Bc250QueuePresentWait, invokes the runtime Present callback,
and then signals Present completion ordering. Device.cpp's queue wait uses the
runtime GPU synchronization callback. This supports the no-CPU-frame-copy design
of that branch. It is not coverage of every resource update or OS-internal path.
The KMD CPU-blit counters and DWM-owned DMA/fence evidence remain necessary.

## Remaining concrete measurement work

1. Add resource/object identity and allocation role to map records, plus map-end
   or live-map state, rather than infer identity solely from dimensions/bind flags.
2. Record uploader suballocation sizes and resource identities, including staging
   callers; keep cumulative counters and produce deltas for an explicit interval.
   Record a final snapshot at the measurement boundary, not only every64 flushes
   or at destructor time (DWM termination need not run the destructor).
3. Bracket deliberate GDI captures with an audit sequence/clock witness. Five
   reads matching five captures in count is not exact per-call attribution.
4. Retain checks for map overflow/unclassified entries, persistent image/user
   pointers, CPU KMD Blt counters and imported primary map/Lock2 paths. Add a
   deliberately copying positive control so the new counters demonstrably detect
   the path they are meant to exclude.

These measurements should accompany the next exact candidate and matched runtime
control. Do not rerun unchanged desktop trials merely to obtain another passing
image; M679 already establishes its bounded pixel and GPU-progress result.
The redirected-blt WSI probe proceeds separately and does not replace this work.

## Prepared audit revisions after M682

M682 adds two short-lived probe windows and exhausts the old128-bucket table:
12 image-map requests/2,592,632 bytes lack bucket identity. The coverage classifier
rejects that trace. Its passing image/DMA controls do not repair the missing audit.

M680 source9f8a1ff9 adds opt-in uploader allocation/copy events. M683 sourcea63dade0
adds growable aggregate map buckets and a per-context lock across counters,
bucket updates and snapshots. Candidate UMD B17855E4 builds; its actual-helper
4097-key/OOM/relocation host control passes. See the M683 fact/evidence entry.
Neither revision has been deployed to the lab. Allocation failure remains explicit
and invalidates coverage, rather than disappearing when the fixed limit is removed.

The resource/lifetime identity, successful-map/unmap tracking, explicit interval
boundary, diagnostic-capture attribution and deliberate-copy controls above remain
unfinished. The growable table retains the existing aggregate key and is not a
substitute for that work.

M684 source150631a8 prepares process/DLL-local resource/object IDs and map
begin/result/end events. A result counts as successful only if its returned
pointer is non-NULL; end does not require a context pointer. The extracted-helper
host control and malformed-trace checks pass; no lab deployment yet. These are
Gallium transfer lifetimes, not persistent Vulkan BO mappings or CPU-store counts.
The new analyzer requires one process/DLL instance per input and cannot detect
whole missing maps. Interval reconciliation, role/capture attribution and the
deliberate-copy control remain open. See M684 for exact artifact and test scope.

M686 source0185cb8d adds a serialized process/DLL event sequence and cumulative
checkpoint counts. BC250_AUDIT_MARKER requests an explicit boundary at a subsequent
Flush, which emits a checkpoint and forces an aggregate snapshot. Four-thread
actual-helper/Windows-futex control passes; the verifier now catches whole missing
maps through both sequence gaps and independent checkpoint count reconciliation.
Use an explicit required end marker, not an arbitrary truncated log tail. A marker
is not a GPU fence or proof of quiescence; pending/live state remains explicit.
This prepared candidate still needs deployment, uploader/capture correlation and
a deliberate-copy control. See M686 and the checkpoint protocol beside the analyzer.
