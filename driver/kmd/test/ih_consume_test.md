# IH consumer empty-interrupt rearm control

Run `powershell -File driver/kmd/test/run_ih_consume.ps1` from the repository.
Outputs remain under the workspace scratch/build/ih-consume directory.
The runner extracts the actual Consume and IhDpc functions from driver/kmd/ih.c and compiles
it with /W4 /WX against a host backend. It does not duplicate the consumer algorithm.

The backend models a serviced MSI as disarmed until pointer publication. An injected
late vector arrives immediately after publication and requires a subsequent DPC.
This is a modeled delivery contract, not a measurement of hardware writeback ordering.
The old equality shortcut fails the first assertion; the revised empty path passes.
The initial negative control used the matching test API at7109741. The current
consumer returns a continuation flag, so older void-returning sources need that
older test runner. Compiler failures never satisfy a negative control.

Additional cases exercise nonempty consumption, ring wrap, backend read failure,
misaligned write pointer, inactive ring, decode error, four-round work bound, and
an overflow recovery position. Kernel concurrency and actual MSI delivery are not
modeled. The shim doorbell implementation and device-stop protocol are unchanged.

PROVENANCE: torvalds/linux, GPL-2.0, revision7d0a66e4bb9081d75c82ec4957c50034cb0ea449; behavioral comparison only, no code copied.
The reference amdgpu_ih_process publishes RPTR even after consuming zero vectors
on its non-overflow path; navi10_ih_set_rptr performs the doorbell write. Our MSI
initialization sets RPTR_REARM. This test does not establish that an empty pass
caused DWM047; that incident still requires hardware validation and OS analysis.

## Post-publication race and continuation

The model also places an arrival after the empty read but before the doorbell,
with either edge or level rearm. Both must consume it without relying on another
unrelated event. Streaming work beyond the budget must return pending; the actual
IhDpc must release consumer ownership, queue exactly one continuation and return.
An empty drained invocation must not queue itself. The previous7109741 consumer
failed the edge pre-publication case before the implementation was changed.

The fast KMD build gate runs this control as `ih-consume` against the selected
worktree, not the main checkout. Post-publication read fault and misalignment
must stop with no continuation; a one-shot overflow must retain recovery position
after its acknowledge clears the overflow indication. There are17 cases.
