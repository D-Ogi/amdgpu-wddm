# RDNA1 packed-fp16 GEMM: barriers that order LDS only

PROVENANCE: our own change to llama.cpp code, MIT (the licence of llama.cpp), not a port. Author D-Ogi.

`llama-rdna1-pkf16-lds-barrier.patch` replaces the two `__syncthreads()` calls of each K step of
`mmf16_rdna1_gemm` with `mmf16_sync_lds()`. In the device pass, that function is a release fence
on LDS at workgroup scope, an `s_barrier`, and an acquire fence on LDS at workgroup scope. The
host pass keeps `__syncthreads()`. The patch applies after `llama-rdna1-pkf16-bload.patch`.

## Why

The threads of a block share data only through LDS. A and B are read-only, and each element of C
has one writer. So the barrier between the stages must order the LDS accesses only.
`__syncthreads()` also orders global memory at workgroup scope. In WGP mode, that fence puts a
`buffer_gl0_inv` after each barrier, two in each K step. Each `buffer_gl0_inv` discards the L0
lines of the A rows that the next K steps read again.

## Measured

Unit A: 40 CU, GPU clock floor 1500 MHz, 12 GiB VRAM carve-out, KMD setting `LargePages` 0, the HIP
runtime of bc250-win `6fe9f6ab` (`amdhip64.dll` 15083FE5), allocation cap 1 GiB. `llama-bench -fa 1
-ngl 99 -p 512 -n 64`, pp512 in tokens per second:

| Build | Qwen3-8B Q8_0 | Qwen3.6-35B-A3B IQ2_M |
|---|---:|---:|
| the ported kernel | 211.6 | 464.0 |
| `llama-rdna1-pkf16-bload.patch` | 235.3 | not measured |
| `llama-rdna1-pkf16-bload.patch` and this patch | 252.1 | 561.6 |
| the two patches and the vector `__hfma2` of bc250-win `2d0f044b` | 251.9 | 657.4 |

The dense Q8_0 kernel has 125 VGPRs with and without this patch. The 35B model has tg64 69.2
tokens per second with the ported kernel and 69.3 with the two patches. The `__hfma2` change is in
`compute/hip/include/hip/hip_fp16.h`, not in llama.cpp. With it, the BN32 IQ2_XXS kernel of the
expert path has 512 `v_pk_fma_f16` in place of 1024 scalar FMAs, at 136 VGPRs.

On the build with the two patches and the vector `__hfma2`, `test-backend-ops` passed 931 of 931
`MUL_MAT_ID` cases. It passed 113 of 113 `MUL_MAT` cases with `type_a=q8_0` and 120 of 120 with
`type_a=q4_K`.

No environment variable selects the old barrier. A build without this patch is the control arm.
