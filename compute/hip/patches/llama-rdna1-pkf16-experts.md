# RDNA1 packed-fp16 GEMM: the mixture-of-experts path

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-pkf16-experts.patch` gives the packed-fp16 GEMM a MUL_MAT_ID path.
`ggml_cuda_mul_mat_id` calls it before MMQ. It uses the compact, expert-sorted ordering that
`ggml_cuda_launch_mm_ids_helper` builds, one expert for each block in z, and reads the activation
column through that map, so it gathers no activations. The column tile of this path is 32.
`GGML_RDNA1_PKF16_ID=0` puts the experts back on MMQ, and `GGML_RDNA1_PKF16_ID_BN` selects 16, 32
or 64 columns.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0012-rdna1-pkf16-expert-path.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` paths in
the commit message and in the code comments are paths of their repository.

## Rebase onto b86d2f07

Applied without a change to the code. The exported patch shows other hunk boundaries in
`mmf16-rdna1.cu` than the source patch, because git chose another diff of the same change: the file
after this patch is byte for byte the file of the source stack. b86d2f07 changed `mmid.cu` (the slot
count padded to a power of two, a case for 10 experts used, a warp barrier). The helper's outputs,
which this path reads, keep their meaning.

## What to measure

Their figures, on Linux at 1500 MHz: qwen3.6-35B-A3B pp512 from 379 to 588 (x1.55). Hypothesis
for this driver. `GGML_RDNA1_PKF16_ID=0` is the A/B arm inside one build.

## Why the default column tile stays 32

MEASURED, unit A, 40 CU, 1500 MHz, 12 GiB carve-out, Qwen3.6-35B-A3B UD-IQ2_M, llama-bench pp512
(t/s). The winner of BN=32 against BN=64 changes with the packed `__hfma2` of the runtime header
`hip_fp16.h`:

| Build | BN=32 (default) | BN=64 |
|---|---:|---:|
| 2026-10-30, before bload, scalar `__hfma2` | 464.00 | 513.94 |
| 2026-10-30, bload and LDS-only barriers, scalar `__hfma2` | 561.59 | 605.61 |
| 2026-10-30, bload, vector `__hfma2` | 657.39 | 604.34 |
| 2026-11-02, this patch set, LargePages 1 (ggml-hip F627D339, BN=64 compiled in) | 661.63 | 610.91 |

With the vector header, the BN=32 IQ2_XXS expert kernel does 512 `v_pk_fma_f16` instead of 1024
scalar FMAs, and BN=32 gains 17 %. BN=64 gains nothing. The shipped runtime has the vector header,
so BN=32 is faster by 8 %. tg64 does not change with the tile width (74.10 against 74.22). The
BN=64 figures of the first two rows are not valid for a build with the vector header.
`GGML_RDNA1_PKF16_ID_BN` keeps the A/B arm inside one build.
