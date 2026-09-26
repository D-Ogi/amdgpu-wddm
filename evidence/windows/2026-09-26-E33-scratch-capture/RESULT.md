# Graphics scratch descriptor capture

PROVENANCE: Mesa MIT; piglit MIT. Unit A, 2026-09-26, KMD151,
Zink D18E2372, diagnostic RADV C05EA103 (base candidate 8B5EC055).

Probe020 uses the bounded2048-element shader from M508 with a configured4x4
FBO. Instrumentation stops deliberately after CPU construction of the scratch
ring descriptor, before creating/submitting the scratch-bearing command stream.
VK_ERROR_DEVICE_LOST and application exit1 are expected instrumentation outcomes,
not a passing pixel test or evidence of a GPU device loss.

Captured allocation:403439616 bytes (384.75MiB),525312 bytes per wave,
768 waves, RADV-reported24 CUs. This CU report does not establish the live CU mask.
The allocation VA is0x2013c0000; descriptor VA0x200039000;
descriptor words013c0000 and80000002 encode the allocation base plus swizzle.
Computed SPI_TMPRING_SIZE is00201300. These are captured values, not MMIO reads.
The descriptor and size are internally consistent; GPU mapping, execution and
scratch correctness remain unproven. M508 TDR root cause is still open.

Loaded-module capture identifies the separate diagnostic ICD. Registry restoration
returned the system to the baseline wsi-final manifest and removed the candidate.
Post-capture health retains generation561767194/epoch5,flags15;1000MHz,VID116,
68.375C. No reset was requested. Source instrumentation was removed after building;
the diagnostic binary remains isolated and must not be promoted.

Next compare graphics scratch setup and the same bounded shader on Linux on this
unit (L37), including actual scratch access and output. Full piglit remains stopped
at its first failing case; this capture does not replace or waive that case.
