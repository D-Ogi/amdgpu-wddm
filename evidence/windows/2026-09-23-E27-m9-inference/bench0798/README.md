# Full WDDM benchmark, unit A, 2026-09-23

Raw outputs unedited. Exact commands in harness and run.cmd; same KMD/ICD as M253/M254. validation.json checks native JSON settings and sample counts. Comparison: evidence/linux/2026-09-21-E14-vulkan-compute-reference/dpm/bench-1000.txt. Linux reports BLAS,Vulkan; Windows Vulkan. Timing is not correctness proof; see separate M254 controls. Final counters are cumulative.


### M255: full-WDDM benchmark baseline

Unchanged0798 and quiet ICD complete b9564 llama-bench pp512/tg128, r3, t6, ngl99 at verified1000MHz/VID116. TinyLlama:1092.70 +/-4.16 prompt tokens/s and113.88 +/-5.00 generated tokens/s. E14 Linux1000MHz reference:1119.59 +/-0.40 and154.92 +/-0.42 (Windows2.40%/26.49% slower). stories15M:37162.42 +/-426.03 and474.49 +/-110.83; generation samples350.30,563.32,509.85 show substantial spread. CumulativeGFX22039/22039 andSDMA150022/150022,zero timeouts/refusals,noTDR;temperature70.6-72.2C. No reboot/reinitialization. Linux and Windows differ in driver/backend stacks; Linux text table does not expose every default and no matched1000MHz stories15M reference is established. Profiling, actual GPU pressure/eviction and remaining audit contracts remain open. See facts M255.
