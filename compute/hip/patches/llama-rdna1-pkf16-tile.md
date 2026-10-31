# RDNA1 packed-fp16 GEMM: a 64-column tile

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-pkf16-tile.patch` changes the tile of the packed-fp16 GEMM of
`llama-rdna1-pkf16-gemm.patch` from 128 rows by 128 columns to 128 rows by 64 columns, and TN to 8.
The author measured 125 VGPRs and eight waves for each SIMD at 64 columns, against 221 VGPRs and
four waves at 128. The tile becomes a template parameter. `GGML_RDNA1_PKF16_TILE` selects one of
five tiles for a test on another part. The arithmetic does not change: each output element sums the
same products in the same order.

Our clang 22.1.8 build agrees: the dense 128x64 instances without prefetch use 125 to 134 VGPRs and
the 128x128 ones 225 to 226. The 128x32 instances of the expert path use 175 to 232. No instance
spills (`scratch/t35/hip-llama/kernel-usage/mmf16-rdna1.txt`, from
`-Rpass-analysis=kernel-resource-usage`).

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0010-rdna1-pkf16-halve-column-tile.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` paths in
the commit message and in the code comments are paths of their repository.

## Rebase onto b86d2f07

Applied without a change. This patch touches only `mmf16-rdna1.cu`, which the port stack adds.

## What to measure

Their figures, on Linux at 1500 MHz: pp512 x1.43 on qwen2.5-1.5B, x1.28 on qwen3-14B, x1.11 on the
MoE. Hypotheses for this driver. `GGML_RDNA1_PKF16_TILE=1` restores the 128x128 tile inside one
build, which is the A/B arm.
