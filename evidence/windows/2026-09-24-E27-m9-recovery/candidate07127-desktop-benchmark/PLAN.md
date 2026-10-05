# KMD127 inference with current desktop baseline

Unit A, retained Windows boot, KMD0.7.127.1 and M412 D3D desktop.
Run the same b9564 llama-bench, model hashes, cache-intent-v2 Vulkan ICD,
ngl99/t6/pp512/tg128/r3 and1000MHz/820mV as M380. Leave the CPU-rendered
llvmpipe/LLVM23 overlay desktop active and record that configuration difference.
Verify exact driver/ICD inputs, native exits and loaded-module witnesses;
collect raw JSON and pre/post completion, paging/capture and TDR counters.
No driver reload, OS reboot or AC operation. Enforce85C and owner STOP.
This establishes a current Windows baseline for later Vulkan changes; it is
not a matched Linux comparison, a speedup claim or full M9 acceptance.
