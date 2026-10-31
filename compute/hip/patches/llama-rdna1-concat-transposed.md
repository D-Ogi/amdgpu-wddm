# Concat of a transposed second source through a shared-memory tile

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-concat-transposed.patch` adds `concat_dim0_transposed` to `concat.cu`. The recurrent
conv input of the Mamba and gated-delta-net graphs is a dim-0 concat of a state and a transposed
view. The generic kernel reads that view one element for each lane at a row stride. The new kernel
moves it through a 32x32 shared-memory tile, so the reads and the writes are both contiguous. The
change is not specific to RDNA1. The patch also adds concat cases with a transposed second source
to `tests/test-backend-ops.cpp`.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0006-concat-transposed-source.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10).

## Rebase onto b86d2f07

Applied without a change.

## What to measure

Their figures, on Linux at 1500 MHz: the op from 10.6 to 3.2 ms on qwen3.8-27B and from 8.3 to
2.5 ms on the qwen3.6-35B MoE, worth 2 to 4 percent of pp together with
`llama-rdna1-gdn-lanes.patch`. Hypotheses for this driver. `test-backend-ops -o CONCAT` on the
lab checks the new cases against the CPU.
