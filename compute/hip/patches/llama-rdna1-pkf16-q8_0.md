# RDNA1 packed-fp16 GEMM: q8_0 too

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-pkf16-q8_0.patch` changes the default of `GGML_RDNA1_PKF16_Q8` from 0 to 1, so q8_0
weights also take the packed-fp16 GEMM for prefill. The author measured q8_0 behind MMQ at the
128x128 tile and ahead of it at 128x64. This patch changes the arithmetic: MMQ quantizes the
activations to 8 bits and accumulates in int32, and this kernel accumulates in f16 with an f32
promotion at each stage. The author measured a perplexity change of +0.078 percent on qwen3-8B
Q8_0. `GGML_RDNA1_PKF16_Q8=0` puts q8_0 back on MMQ.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0011-rdna1-pkf16-q8_0.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` paths in
the commit message and in the code comments are paths of their repository.

## Rebase onto b86d2f07

Applied without a change. This patch touches only `mmf16-rdna1.cu`.

## What to measure

Their figure, on Linux at 1500 MHz: qwen3-8B Q8_0 pp512 from about 260 to about 379. Hypothesis
for this driver. `GGML_RDNA1_PKF16_Q8=0` is the A/B arm inside one build, with a short perplexity
check next to it.
