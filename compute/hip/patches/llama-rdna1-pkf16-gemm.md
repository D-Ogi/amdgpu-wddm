# RDNA1 prefill through a packed-fp16 tile GEMM

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-pkf16-gemm.patch` adds `mmf16-rdna1.cu`, a tile GEMM for prefill on RDNA1. gfx1013
has no `v_dot4_i32_i8`, so MMQ emulates its int8 dot products. This kernel dequantizes a block of
weights into shared memory as f16, converts the f32 activations to f16 while it stages them, and
multiplies with `v_pk_fma_f16`. It accumulates in f16 within a stage and promotes to f32 at the
end of each stage. `ggml_cuda_mul_mat` calls it before MMQ when the types and the shape are
supported: q4_K, q5_K, q6_K, IQ2_XXS, IQ3_XXS, IQ3_S and IQ4_XS, with enough rows and columns.

The patch reads these environment variables: `GGML_RDNA1_PKF16=0` turns the kernel off,
`GGML_RDNA1_PKF16_PROMOTE` sets the stages between promotions, and `GGML_RDNA1_PKF16_DBUF=1`
prefetches the next tile. The defaults are the values the author measured as best.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0008-rdna1-pkf16-prefill-gemm.patch`, last changed in their commit `bd897eccac05cb02f1c0c1e3cb9d4e6a490f58b2` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` paths in
the commit message and in the code comments are paths of their repository.

## Rebase onto b86d2f07

Applied without a change: the changed lines are the same as in the source patch, and only the
line offsets differ. The later patches `llama-rdna1-pkf16-tile`, `-q8_0`, `-experts` and
`-batch128` change this kernel. At the end of the stack `mmf16-rdna1.cu` and `mmf16-rdna1.cuh`
are byte for byte the files of the source stack.

## What to measure

Their figures, on Linux at 1500 MHz: pp512 on qwen2.5-1.5B from 793 to 1250, on the 14B models
about +50 percent. These are hypotheses for this driver. The lab measures pp512 against the build
without the ports, and `GGML_RDNA1_PKF16=0` is the control arm inside one build.
