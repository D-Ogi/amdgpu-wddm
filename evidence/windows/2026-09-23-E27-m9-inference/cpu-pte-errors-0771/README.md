# CPU PTE failure handling, local0771

VidMmUpdatePageTable now returns NTSTATUS internally. It validates call shape,
encodes all entries into a bounded paged-pool snapshot, and only then maps/writes
the destination. A refused later entry does not write an absent PTE or leave a
valid prefix in the destination. Repeat and StartIndex are preserved. Allocation
or physical mapping failure returns INSUFFICIENT_RESOURCES, closed write gate
returns DEVICE_NOT_READY, invalid source/shape returns INVALID_PARAMETER.
Snapshot and owned mapping are released on every exit; CPU_VIRTUAL uses the
OS-provided mapping and does not unmap it. Successful writes use a memory barrier.
Catchable exceptions propagate, but this is not rollback of partial writes caused
by an invalid destination pointer. The OS must keep kernel pointers valid.

GfxPagingBuildUpdate's pre-RUN bootstrap propagates the CPU status internally and
advances NextEntry only after success. It still never falls back to CPU after
bootstrap closes. Snapshot allocation avoids adding4KiB to the kernel stack;
this adds a bounded transient allocation to immediate CPU initialization.

Validation: extracted actual CPU updater replaces prior stub in the routing
harness, real AMD translator/encoder/builders, mocked allocation/map interfaces.
6210checks pass. New cases cover physical success/canaries, map and allocation
failure, bootstrap zero progress on failure, invalid final entry leaving entire
512-entry table unchanged, direct CPU_VIRTUAL Repeat, closed gate/invalid inputs,
and balanced maps/pool/locks. No SEH exception injection or kernel lifetime proof.
Existing packet/routing/multipass controls also pass.

Full WDK build/sign PASS, candidate0.7.71.1 NOT DEPLOYED:
scratch/build/bc250kmd-0771/package-umd.
SYS SHA256195D7A2F42CC738FA028D1A4ABC5DD35C0B1A8308BA20B733DA49F387FBBF022.
No lab access, reset, agents or USB changes.

IMPORTANT: the outer BuildPagingBuffer still ignores CPU_VIRTUAL errors and
folds GPU/helper failures into empty SUCCESS. This revision fixes internal error
visibility and partial writes, NOT the full OS error contract. Arbitrary helper
NTSTATUS values cannot simply be returned by that restricted DDI. Completion,
bootstrap fault policy, hardware lifecycle and real paging acceptance remain open.
