# M482: ordered sparse mapping batches and ordinary Vulkan control

Unit A and host model, 2026-09-25. M12.1 remains open.

## Implementation
Mesa05e6c962 plus the pinned full WDDM2 patch now builds one array of mapping
operations per sparse submission. RADV queues application semaphore waits,
then a monitored-fence boundary after previous rendering. UpdateGpuVirtualAddress
waits on that boundary and signals boundary+1. The rendering queue waits for
that completion before subsequent rendering or application signals.

A local submission copy prevents consuming the same application waits twice
while retaining runtime-owned data for cleanup. Sparse-only submissions use
the GPU queue for signals as well. Empty batches retain input dependencies;
failed batch construction discards unsubmitted operations. The Linux winsys,
which does not provide these optional transaction callbacks, retains its path.
This adds no CPU wait per mapped page. Initial reservation still waits for its
initial OS paging fence, as recorded in M481.

Contract checked in local WDK26100 D3DKMT_UPDATEGPUVIRTUALADDRESS and
Microsoft display/tile-resources.md at110f60ea. This implements the caller-side
ordering, not the missing native PRT/Zero and privileged PTE-copy behavior.

## Host test
test_sparse_order.py extracts the actual begin/append/end and map/unmap
functions. MSVC compiles them with real WDK declarations and mock OS calls.
A two-queue scheduler model checks old-page rendering before the remap,
new-page rendering afterward, delayed and already-signaled application
semaphores, both legacy-context and HW-queue API branches, one257-operation
batch, fence progression1-4, and empty/cancelled batches. All scenarios pass.
The mock does not establish Windows scheduler or GPU hardware behavior.

## Unit A control
Candidate SHA256:
255534CA83EC6141434C3BB241C99FD7D8D10DDF32AC04979B4B6DADC5668E4A.
Normal-user explicit manifest selects this candidate. Process module witnesses
confirm it together with the system Vulkan loader. vulkaninfo exits0, all8
compute cases match CPU/reference hashes, and vkcube exits0 after600frames.

Run12:28:32Z-12:29:00Z. Readback: same11:20:11Z Windows boot,
flags15/generation590515602/epoch5, native1000MHz/VID116 and66.125C.
No restart was used. The readback does not include a fresh event/dump audit.
System registration remains9C40083C; the candidate is not the system default.

Sparse remains disabled. These ordinary GPU controls do not exercise sparse
map/unmap on hardware; release CTS, AI/image controls and Linux parity remain
open. Multi-queue use beyond the currently exposed single graphics queue also
requires review of the existing per-IP context model.

## Reproduction and remaining work
The full patch and SHA pin are in experiments/E33-m12-applications.
controls.zip contains host source, compiler output, build identity and lab
outputs; manifest.json records original/published hashes. UUID/LUID values only
are redacted. Next: native Zero/PRT and scalar alias policy, current PTE lookup
during queued copies, buffer/image content checks and matched Linux tests.
PROVENANCE: Mesa and upstream WDDM2 integration are MIT.
