# Staged PTE copy packet construction

2026-09-23, HEAD bed764d plus worktree. Host tests only; no lab access/deployment.

bc250_sdma_paging_copy_ptes composes existing AMD copy emission and fence/poll
pipeline barriers: source->staging, barrier, staging->destination, barrier.
Counts1..512 entries cover the current4KiB table shape. Caller supplies resolved
MC addresses, a disjoint retained4KiB staging page, marker storage outside all
ranges, and two fresh nonzero sequence values. All address-end/alias checks and
whole-transaction capacity reservation precede output. No allocation, mapping,
TLB invalidation, doorbell, OS fence completion or logical-state publication.

Actual packets occupy34 DWORDs; ring reservation aligns to48. The caller must
also budget the outer completion fence and account for accumulated buffer bytes.
The temporary constructor ring is bounded to48 even for a larger input capacity.
Every capacity0..47 refuses with48 required and leaves the buffer unchanged.

Validation:8493 combined packet/routing checks pass, including M191. For208 valid
source/destination/count combinations (including overlapping and self-copy),
the test interprets the two copy payloads in host memory and compares destination
content to an independent memmove oracle. It checks exact copy fields, distinct
marker/poll values, packet offsets, guards and invalid input/alias/overflow/wrap.
This is NOT GPU execution, barrier timing, cache coherency or live resource proof.
Initial test oracle omitted AMD fence MTYPE3; fixed oracle to match the existing
AMD-backed emitter, no emitter behavior change was required.

Full dev build/sign passes, scratch/build/pte-copy-packets-dev retains0775 and
must NOT be deployed. SYS SHA256:
8E9FB3D87A756CFF9E5D57FF9683D61EAE45A2F61219FF9575BC05E26098FC4E.
Official0775 candidate unchanged; lab last verified0773 with gates closed.

Remaining: reserve staging resource at engine setup with teardown ownership,
resolve DXGK copy-range GPU VAs, support range/multipass capacity and ordering,
publish logical metadata for accepted commands, and establish hardware behavior.
Do not infer native overlapping SDMA memcpy behavior from the two-stage model.
The previous M195 helper's logical snapshot semantics must match GPU execution.
Existing AMD import provenance: Linux v6.18 commit
7d0a66e4bb9081d75c82ec4957c50034cb0ea449, AMD portions MIT.
