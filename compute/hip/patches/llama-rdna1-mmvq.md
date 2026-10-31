# RDNA1 matrix-vector kernels for decode

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-mmvq.patch` is the decode change of the port. It has three parts, and each part is
behind an RDNA1 check:

- An RDNA1 entry in the launch table of the quantized matrix-vector kernel (`mmvq.cu`). Without
  it, gfx1013 uses the generic entry: four warps for each row, with a shared-memory exchange and
  a barrier. The entry gives one wave for each row to the K-quants and the IQ types, four warps
  to Q4_0, Q4_1, Q5_0, Q5_1 and Q8_0, and four warps again to the K-quants when K is 8192 or more
  (the `long_k` flag).
- `ggml_cuda_sum_i8x4` in `vecdotq.cuh`. It sums four int8 values with `v_sad_u8` in place of the
  dp4a emulation, in the q2_K, q4_K and q5_K paths. The arithmetic is exact.
- `mmvq-rdna1-f32.cu`, a new kernel for one column. It reads float activations, so it does not
  quantize them to q8_1 and does not use the emulated int8 dot product. It covers q4_K, q5_K,
  q6_K, q8_0, iq2_xxs, iq3_xxs and iq3_s, the bias, gate and GLU fusion, and MUL_MAT_ID for one
  token.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0005-rdna1-mmvq-table-and-sums.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |
| Our base | `b86d2f07` with the four bc250 patches before it in `llama-patches.json` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` paths in
the commit message and in the code comments are paths of their repository.

## Rebase onto b86d2f07

This is the one port with changes of our own. b86d2f07 added a GB10 entry to the same table and
a `halve_iters` flag next to `small_k`, so the source patch did not apply. The changes:

- `long_k` is the last flag everywhere: `calc_nwarps`, the `mul_mat_vec_q` template and its
  `__launch_bounds__`, `calc_launch_params` and `mul_mat_vec_q_switch_fusion`. The enum keeps
  `MMVQ_PARAMETERS_GB10` and adds `MMVQ_PARAMETERS_RDNA1` after it.
- b86d2f07 launches the one-column case through a lambda with compile-time tags. The port adds a
  third tag for `long_k`. The order of the selection is small K first, then the RDNA1 long row
  (`ncols_x >= 8192`), then the GB10 `halve_iters`, then the default. The source patch has the
  same order, and `halve_iters` is never true on RDNA1.
- b86d2f07 added a shared-expert fusion (`shared_up`) and the `SWIGLU_CLAMP` GLU operation. The
  float-activation kernel knows neither. The port sends those two cases to the int8 path, so they
  stay correct and keep the speed of b86d2f07. A MoE model with a fused shared expert therefore
  does not get the float kernel for that product. That is a gap to measure, not a defect:
  `GGML_CUDA_DISABLE_FUSION=1` turns every fusion off, and a decode A/B with and without it
  shows how much of the float kernel the shared-expert fusion takes away.

The new files `mmvq-rdna1-f32.cu` and `mmvq-rdna1-f32.cuh` are byte for byte the source patch's
files at the end of the stack (after `llama-rdna1-iq-codebook.patch`).

## What to measure

Their figures, on Linux at 1500 MHz against their three-patch build: decode x1.5 to x2.05. These
are hypotheses for this driver, not facts. The lab measures tg on the six-model set of
`scratch/t35/hip-llama/TARGETS.md` against the build without the ports.
