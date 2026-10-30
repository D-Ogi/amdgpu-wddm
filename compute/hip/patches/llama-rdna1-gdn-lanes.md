# Gated delta net lanes per column on RDNA1

PROVENANCE: llama.cpp patch by Artur Andrzejczak, MIT (the licence of llama.cpp), from https://github.com/akandr/bc250-rocm.

`llama-rdna1-gdn-lanes.patch` makes the lane count for each column of `gated_delta_net.cu` a
template parameter (`lanes_t`). Every other architecture keeps the upstream geometry, one wave for
each column. On RDNA1, for S_v = 128 and more than one token, eight lanes share a column, which is
the geometry of ggml-vulkan's shader. At one token RDNA1 keeps the upstream form, which the author
measured as faster. `GGML_GDN_LANES` overrides the prefill lane count for tests. The reduction
order changes.

## Source

| Item | Value |
|---|---|
| Repository | https://github.com/akandr/bc250-rocm, read at commit `9a2abb17834ffefbadd3312926fa83571f3f7b08` |
| File | `patches/llamacpp/0007-rdna1-gdn-lanes.patch`, last changed in their commit `4dc3dfc4aafc451143e334f243efaad15ed9d990` |
| Author | Artur Andrzejczak, kept in the `From:` header and the `Signed-off-by:` trailer |
| Their llama.cpp base | `7ba604f1cb61cd14898138e9abc0b4ff2601f180` |

We removed the AI co-author trailer of the source patch (repository rule 10). The `logs/` path in
the commit message is a path of their repository.

## Rebase onto b86d2f07

Applied without a change.

## What to measure

Their figures, on Linux at 1500 MHz: the 2048-token op from 19.8 to 7.1 ms on qwen3.8-27B and from
10.2 to 7.1 ms on the qwen3.6-35B MoE, pp512 +1.5 to +3.4 percent. Hypotheses for this driver.
Only models with gated delta net layers (the qwen35 architectures) reach this kernel.
