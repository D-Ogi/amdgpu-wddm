# RDNA1 packed-fp16 GEMM: 128-token batches

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-pkf16-batch128.patch` lowers the smallest batch that takes the packed-fp16 GEMM from
256 tokens to 128. The row threshold stays at 512. Both thresholds become environment variables,
`GGML_RDNA1_PKF16_MINCOLS` and `GGML_RDNA1_PKF16_MINROWS`. Batches of 256 tokens and more do not
change. This is for short prompts.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0013-rdna1-pkf16-admit-128-token-batches.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` paths in
the commit message and in the code comments are paths of their repository.

## Rebase onto b86d2f07

Applied without a change. This patch touches only `mmf16-rdna1.cu`.

## What to measure

Their figures, on Linux at 1500 MHz: a 128-token batch x1.08 to x1.24 against MMQ. Hypothesis for
this driver. `llama-bench -p 128` with `GGML_RDNA1_PKF16_MINCOLS=256` is the A/B arm inside one
build.
