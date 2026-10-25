# AGS private-query replay and ADL review

2026-10-25. AGS SDK 6.3.0, x64 and x86. The test client intercepts only
D3DKMTEnumAdapters and D3DKMTQueryAdapterInfo lookups in its loaded AGS image.
The ADL test snapshot contains two AMD adapters and three displays. No system
library, registry value or physical adapter is changed.

## Controlled comparison

The original ADL test library is kept identical in both arms. The old KMD policy
replies to a 17248-byte private query with the exact umd_caps_blob. The bounded
policy refuses that query. Both arms use callback allocations with a trailing
no-access page (up to 15 bytes of alignment padding).

| Architecture | Old policy under debugger | Bounded policy under debugger |
|---|---|---|
| x64 | first-chance AV, AGS RVA 0x19969, caller 0x5ec3 | AGS_SUCCESS, two AMD GPUs, zero live callback allocations |
| x86 | first-chance AV, AGS RVA 0x6fec | AGS_SUCCESS, two AMD GPUs, zero live callback allocations |

The normal host suite also passes all 12 selected cases in each architecture.
It includes guarded private-query info and 20-round tests, 100-round ADL tests,
missing-ADL and missing-export controls, and D3D12 clear/readback on the host GPU.
It does not establish a lab rendering result.

## Review findings

Public ADL SDK revision: 84c881cb2136f04287eb3af12de00a9e2867aa9e2867aa9e.
PROVENANCE: GPUOpen-LibrariesAndSDKs/display-library, MIT.

- The compile gate compares 15 public structure sizes, alignments and selected
  member offsets in both architectures. Export names and x86 calling conventions
  pass. The crash is not caused by ADL structure packing.
- Four array functions called the caller's allocator while holding the global
  exclusive SRW lock. A callback that called ADL again deadlocked. Commit 86a49c52
  snapshots the data and callback under the lock, then releases it before calling
  the allocator. Query, refresh and context destruction inside that callback pass.
- On failure of the second DisplayMapConfig allocation, the first array remains
  caller-owned. Its pointer stays visible. The library cannot free an allocation
  from an arbitrary callback without a matching free callback.
- Unit tests pass 3206 checks in x64 and 3241 in x86. The new reentrancy tests
  fail within their bound against the old library, in both architectures.

Reproduce the positive replay with test-umd-adl-ags.ps1 after build-umd-adl.ps1.
For the old-policy control, run `ags-client.exe info --kmt-legacy --guard-allocations`
under a debugger configured to stop on first-chance AV and fail-fast. The matching
positive command uses `--kmt-fixture` instead of `--kmt-legacy`.
