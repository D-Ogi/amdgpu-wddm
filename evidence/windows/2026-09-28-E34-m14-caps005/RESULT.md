# Caps005: Windows Vulkan budget query and Trim pass

PROVENANCE: Mesa, MIT; local WDDM2 source tree.

Exact engine DC65/test5D7D unchanged from failed controls M749/M750. The new ICD is
C38885C50A0A1A77B29281F0BB3D78196BF228765BD22AB93FDBB35ECBB85BCC,
with an in-process module hash witness. MSVC/Ninja build passed.

The incremental driver/icd/mesa-wddm2-budget-query.patch follows
mesa-wddm2-memory-accounting.patch. It replaces the Windows zero-only heap query:
allocation counters are summed over live winsyses matching the adapter LUID while
the creation mutex protects enumeration/lifetime. Counters remain atomic during BO work.
Initial-domain accounting follows amdgpu, with imported/borrowed wrappers included
and sparse virtual reservations excluded. Failed allocation destruction retains its charge.
Source hashes identify each changed file before and after LF normalization.

Unchanged test: usage80MiB before,588MiB after256 one-MiB buffers,588MiB after
release,17MiB after Trim. These totals include engine pools and other allocations;
they are not the sum of requested payload sizes alone. Both Trim assertions pass,
along with the engine OOM/recovery/render suite (PASSED:0 failures).
Engine10.555s, supervisor27.918s, no timeout, root exit0, Job empty.
CPU171 baseline and boot/DWM/device-generation postflight unchanged,67C.

Caps are admitted only for this exact engine/ICD pair, maximumFL11_1. Generated
108-byte config SHA256 C75C2B6E6A4354F80E4721951060C7138F331E604E84496710BC0752FB5C8166.
No native UMD deployment occurred during this test. Native deferred-error propagation,
multiple hosted sessions, multi-adapter isolation and concurrent lifecycle stress still
need their own controls. This test does not establish D3D12 or FL12_1 acceptance.

The existing global-usage mapping still uses KMT CurrentUsage. OS Budget integration
and fully process-wide accounting across separate loaded ICD modules remain outside
this patch; no claim that all heapBudget semantics are now correct.
Raw logs stay in scratch/m14/lab-caps005; selected memory lines are verbatim.
Closure omits the process identifier; no allocation handles/private identifiers exported.
