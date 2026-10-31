# RDNA1 IQ matvec: the codebook in shared memory

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-iq-codebook.patch` changes the IQ2 and IQ3 decoders of the float-activation matvec of
`llama-rdna1-mmvq.patch`. A block copies the codebook into shared memory before its loop, so each
lookup is an LDS read and not a second dependent global load. The expert-id path covers too few
superblocks for each block to pay for the copy, so only the ordinary matrix-vector products stage
the codebook. The patch also replaces six 8-bit loads with three 16-bit loads for the codebook
indices and the iq3_s sign bytes. The arithmetic does not change.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0009-rdna1-iq-matvec-shmem-codebook.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` path in
the commit message is a path of their repository.

## Rebase onto b86d2f07

Applied without a change. This patch touches only `mmvq-rdna1-f32.cu`, which the port stack adds.
No instance of the kernel spills with our clang 22.1.8. The largest uses 94 VGPRs
(`scratch/t35/hip-llama/kernel-usage/mmvq-rdna1-f32.txt`).

## What to measure

Their figure, on Linux at 1500 MHz: decode +1.8 percent on qwen3.8-27B (14.78 to 15.04 tokens per
second), no change on the MoE. This is a hypothesis for this driver. It is the smallest gain of
the stack.
