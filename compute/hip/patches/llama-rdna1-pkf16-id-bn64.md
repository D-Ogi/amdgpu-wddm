# RDNA1 packed-fp16 GEMM: MoE expert tile default BN=64

`llama-rdna1-pkf16-id-bn64.patch` changes the compiled default of the expert (MUL_MAT_ID) column
tile width from 32 to 64. The environment variable `GGML_RDNA1_PKF16_ID_BN` still overrides it:
values 16, 32 and 64 are accepted, any other value falls back to 64.

## Why

The MoE expert path dispatches one expert per block in the z dimension. At a 512-token batch
with 8 of 256 experts used, each expert sees about 16 tokens on average, so wider tiles pad more.
The original default of 32 was a conservative choice before lab measurements.

## Measured numbers (BC-250, 40 CU, 1500 MHz, 12 GiB, on bload+ldsbar build)

Qwen3.6-35B-A3B IQ2_M pp512 (the only MoE model in the test set):

| BN | pp512 t/s |
|---:|---:|
| 16 | 393.2 |
| 32 | 491.6 |
| 64 | 513.9 |

BN=64 is 4.5 % faster than BN=32 and 30.7 % faster than BN=16 on this model. The tg64 numbers
are unchanged (matvec, not GEMM). With LargePages 1 and vector `__hfma2`, the 35B reaches 605.6
at BN=64.

The env override stays so a different part or model can still test other values.
