# M481: sparse VA reservation groundwork and current Mesa control

Unit A and host control, 2026-09-25. M12.1 remains open.

## Source change
Ported the existing BC250/WDDM2/CPU-WSI integration to Mesa
05e6c9622e135ac2aeaf56ec70222642627e2162. The old source, binaries and
system registration remain available. Complete replayable source:
experiments/E33-m12-applications/mesa05-wddm2.patch, pinned by mesa05-source.json.

Corrected the virtual BO reservation path against local WDK26100 declarations:
use the adapter handle in ReserveGpuVirtualAddress, honor replay addresses and
alignment, preserve the complete OS reservation separately from an aligned
interior resource address, record the BO size, wait for initial mapping's paging
fence, and free the original complete reservation. Map failure releases the
reservation. Sparse remains disabled pending the remaining implementation.

## Host control
test_sparse_reservation.py extracts the actual reserve/create functions and
the actual destructor's FreeGpuVirtualAddress call. It compiles against the
WDK's D3DKMT declarations and uses a mock OS dispatch to check:
- 64KiB resource creation and complete release;
- 512KiB alignment from a deliberately unaligned64KiB OS base;
- 5GiB virtual reservation without an artificial4GiB limit;
- exact replay VA reservation;
- STATUS_PENDING map followed by a wait on its returned paging fence before
  BO creation returns.

All four positive controls pass with no compiler warning. This proves the tested
request construction and lifetime bookkeeping, not actual GPU sparse behavior.

## Lab control
Candidate SHA256:
80FB08169865CEAEB25094F695DFF69AC7D9049BACD7C1707D5E620CB130AA28.
The normal-token worker explicitly selects its manifest. Every process module
witness identifies that candidate and the System32 Vulkan loader.
vulkaninfo completes; all8compute cases match their CPU/reference hashes;
vkcube completes600frames with native exit0.

Interval12:10:56Z-12:11:25Z. Final readback retains Windows boot11:20:11Z,
flags15/generation590515602/epoch5 and native1000MHz/VID116,65.875C.
No OS/DWM/GPU restart was used. This readback did not perform a fresh event-log
or crash-dump audit; do not infer one.

The system-wide default remains the previously measured9C40083C ICD.
This candidate has not yet passed the release CTS group, AI models, image
oracle or matched Linux checks. No performance acceptance follows from the
single cube elapsed time. Vulkan sparse features remain disabled, so this
control does not exercise the new reservation path on hardware.

## Remaining M12.1 work
- Native zero/PRT semantics at the required page-table levels and the GFX10
  scalar-load alias workaround, with ordinary/replay VA ranges respecting it.
- Ordered mapping batches: current vm_fence.wait_value is never advanced by
  virtual_bind, and the rendering queue does not wait for mapping completion.
  Connect input semaphore waits, prior rendering, mapping completion and later
  rendering/signals before enabling sparse capabilities.
- Privileged paging-context execution and current PTE resolution for queued
  CopyPageTableEntries; test remapping while work is queued.
- Bind/unbind/rebind content checks for buffers/images and own-unit Linux parity.

controls.zip preserves successful raw controls and the actual-function host
test. Only UUID/LUID values are redacted. manifest.json records both hashes.
PROVENANCE: Mesa and upstream WDDM2 integration are MIT.
