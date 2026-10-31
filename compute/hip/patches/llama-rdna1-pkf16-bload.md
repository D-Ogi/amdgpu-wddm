# RDNA1 packed-fp16 GEMM: B tile loads in groups of four

PROVENANCE: our own change to llama.cpp code, MIT (the licence of llama.cpp), not a port. Author D-Ogi.

`llama-rdna1-pkf16-bload.patch` changes how `mmf16_rdna1_gemm` of `llama-rdna1-pkf16-gemm.patch`
loads the B tile, the f32 activations. The new function `mmf16_load_b` has a constant trip count:
16 loads for each thread at the 128x64 tile. It sends four global loads, then converts the four
values to f16 and writes them to LDS. The loop over the groups of four does not unroll. A column
past the end of the batch loads the last valid column and stores zero, so no lane branches around
a load. The arithmetic does not change.

## Why

The loop of the ported B loader started at the thread index. LLVM 22 did not unroll it. Each of
its 16 iterations sent one `global_load_dword` and then waited for it with `s_waitcnt vmcnt(0)`.
So each wave had one load in flight in the B loader, in each K step of every packed-fp16 kernel.

A full unroll sends the 16 loads together, but the kernel then needs 195 VGPRs. That gives five
waves for each SIMD in place of eight, and the kernel was slower than with the serial loader.
Groups of four keep the kernel at 125 VGPRs and eight waves for each SIMD.

## Measured

Unit A: 40 CU, GPU clock floor 1500 MHz, 12 GiB VRAM carve-out, KMD setting `LargePages` 0, the HIP
runtime of bc250-win `6fe9f6ab` (`amdhip64.dll` 15083FE5), allocation cap 1 GiB. `llama-bench -fa 1
-ngl 99 -p 512`, Qwen3-8B Q8_0, pp512 in tokens per second:

| B loader | VGPRs | pp512 |
|---|---:|---:|
| serial, as ported | 125 | 211.6 |
| all 16 loads in flight | 195 | 161.5 |
| groups of eight | 119 | 156.4 |
| four 16-byte loads for each thread | 138 | 155.3 |
| groups of four, this patch | 125 | 235.3 |
| groups of four and `llama-rdna1-pkf16-lds-barrier.patch` | 125 | 252.1 |

A second run of the serial loader, in the same session as the run with groups of four, gave 211.8
tokens per second. With groups of eight and with 16-byte loads, the compiler put the A tile loads
back into a serial chain.

No environment variable selects the old loader. A build without this patch is the control arm.
