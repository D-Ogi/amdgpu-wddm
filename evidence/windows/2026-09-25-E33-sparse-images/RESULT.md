# M487: sparse image reservation geometry and release image controls

Unit A,2026-09-25. KMD149 remains93ECB1BE; Mesa05 candidate
0F9FEAE8492B71343B7DBDE6A12DE9E1D95464C3AB20D0ABE45E35E383F7BF2D.
System ICD9C40083C is restored after both experimental CTS runs.

## Failure and correction
On M485 candidate17ADD01F, the1024x128 sparse-binding image passes, then11x137
fails at vkCreateImage with VK_ERROR_OUT_OF_DEVICE_MEMORY. RADV can request
4KiB-aligned sparse image resources. Our reserve helper incorrectly required
the resource size itself to be a64KiB multiple.

WDK26100 D3DDDI_RESERVEGPUVIRTUALADDRESS.Size requires a64KiB reservation.
The corrected helper rounds only the OS reservation and retains4KiB resource
mapping/bind lengths. High alias reservations/free calls use the same rounded
length; resource size is unchanged. Host actual-source tests cover4KiB and68KiB
resources,64KiB, interior512KiB alignment,5GiB and exact replay. The75-file
port patch is replay-checked against05e6c962.

## Release CTS results
vulkan-cts-1.4.6.2, f6a29701220f34dd1407513bfe80d74ca7b392ce,
with explicit RADV_EXPERIMENTAL=sparse and loader/module witnesses:
- RGBA8 2D images:20Pass/1NotSupported. Covers ordinary binding, partial
  residency, rebind, aliases, mipmaps and shader sparse reads, including
  unaligned image dimensions.
- The128x128 image_rebind case is NotSupported because it is too small for
  partial binding (vktSparseResourcesImageRebind.cpp:333); larger rebind cases pass.
  This is the CTS geometry prerequisite, not evidence that rebind is unsupported.
- Same21buffer controls from M485 repeated on the new candidate:21Pass.
- Combined:41Pass/1NotSupported/0Fail,15:37:13Z through15:38:55Z.
- Same boot14:50:35Z, generation559046836/epoch5,1000MHz/VID116.
  Final readback has no new selected fault events or dumps.

The CPU-only captured gallery remained visible; these are correctness runs,
not performance measurements. Full logs, QPA, runner source, selections and pins
are in controls.zip. Device identifiers are redacted; originals remain outside
the repo. No framebuffer image, firmware or memory-dump payload is included.

## Remaining acceptance
This selection does not establish full sparse image/format coverage or Linux
parity. Privileged CopyPageTableEntries still needs execution-time translation
rather than captured physical addresses. General Vulkan sparse stays disabled
by default. Full M12 also requires application/API/performance comparisons;
none is implied by these42selected tests.
