# RDNA1 flash-attention tile rows without spill

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-fattn-tile.patch` adds `ggml_cuda_fattn_tile_get_config_amd_rdna1` to `fattn-tile.cuh`,
a tile table that RDNA1 uses before the shared RDNA table. RDNA1 has no `v_dot2_f32_f16`, so
`ggml_cuda_mad()` unpacks each half2 into two floats, and the D=128 tile kernels of the shared
table spill registers there. The table sets rows for D=128, D=256 and D=512. The patch also changes
two choices:

- A model whose GQA ratio has no power-of-two factor gets `ncols2 == 1`. On RDNA1 such a model
  stays on the 32-column tile for prefill, because the 64-column instance spills.
- For one token, such a model goes to the vector kernel on RDNA1 (`fattn.cu`).

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0004-rdna1-fattn-tile-no-spill.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` paths in
the commit message and in the code comments are paths of their repository.

## Rebase onto b86d2f07

Applied without a change. b86d2f07 changed rows of the NVIDIA table and added an argument to
`launch_fattn`. Neither change touches the RDNA1 rows or the two choices above.

Register use with our clang 22.1.8, D=128 (`scratch/t35/hip-llama/kernel-usage/`
`fattn-tile-instance-dkq128-dv128.txt`): the instances without logit softcap that RDNA1 launches do
not spill. Five instances still spill VGPRs: the 64x1 instances (with and without softcap, which
the first choice above keeps RDNA1 away from) and the softcap instances 32x2, 16x4 and 8x8. Only a
model with logit softcap reaches the last three.

## What to measure

Their figures, on Linux at 1500 MHz: pp2048 +40 percent on D=128 models with GQA sharing, and
prefill at depth on qwen3-8B Q8_0 394.4, 300.5 and 214.6 tokens per second at depths 0, 4096 and
8192. Hypotheses for this driver. The lab measures pp2048 at those depths, one `llama-bench` call
for each depth.
