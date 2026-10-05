# M264: repeated shader visibility on full WDDM0798

2026-09-23, unit A. Installed KMD0.7.98.1 unchanged; no reboot, reinitialization or driver installation. Source base bed764da5192be7132d646be0e6331c1edeb30fd, with exact probe/reference source snapshots under source/.

Probe SHA25648A286FCE6256BF711B71CF11DA855194434C8ED80CCBE35AA3B21AF5A4D1328. MSVC /W4 /WX /O2 /std:c11 /wd4702 build/help pass; C4702 suppression covers inherited E14 unreachable fallback after fatal exit. Actual quiet ICD loader witnesses appear in all three stderr files; DLL SHA2564E05F1DF627CD6D9D64FE7F1ADDA29673D5B3B96B3750A08034A7EE88460C9EA. Limited interactive scheduled task, removed after completion.

Three Vulkan allocations remain mapped/reused within each process. Sixteen rounds rewrite1M input words and intermediate/output sentinels, execute two chained inthash dispatches, then compare all output bytes against two CPU mix32 applications. Existing E14 barriers cover shader-write/shader-read and shader-write/host-read; each round waits for a fence before CPU accesses. Host-visible/coherent memory is requested; no explicit eviction is performed.

Results: positive16rounds/0mismatches/exit0; intentional stale-input control passes round0 then detects round1 mismatch/exit1; fresh-process repeat16rounds/0mismatches/exit0. Host validator verifies16 distinct hashes, exact repeat agreement and the negative control retaining the previous GPU hash while the expected hash advances.32 positive rounds,128MiB final output checked. This is correctness evidence, not a performance comparison.

GFX30507->30541 submissions/completions (+34), SDMA670556->673837 (+3281), zero timeouts/refusals and no TDR before/after. Clock query1000MHz,VID116(request820mV); not an analog voltage measurement. Monitored temperature71.0..71.5C. No active GPU job remains.

Scope: positive shader visibility for these reused host-coherent buffers through this Vulkan stack. Does not prove OS page-table CPU cache attributes, arbitrary NC/WC aliases, eviction-plus-shader visibility, every memory type, legacy aperture callbacks, or full M9 completion. M259-M262 source changes remain undeployed. Kernel logs are UTF16; validator handles their BOM. No privacy redactions were required in these captured artifacts.
