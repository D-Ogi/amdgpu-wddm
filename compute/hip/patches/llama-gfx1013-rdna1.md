# gfx1013 belongs in the RDNA1 list

PROVENANCE: llama.cpp, MIT, upstream base b86d2f07542b29ab099aed34fd6b6d1b2fd4b81c.

`llama-gfx1013-rdna1.patch` names `__gfx1013__` in the device-side `RDNA1` definition of
`ggml/src/ggml-cuda/vendors/hip.h`. Upstream lists `__gfx1010__` and `__gfx1012__` only.

## Why the build needs it

The host side already treats gfx1013 as RDNA1, because `GGML_CUDA_CC_IS_RDNA1` is a range test and
"gfx1013" parses into it. The device side did not, so the two disagreed about the launch geometry:
the device compiled `mul_mat_vec_q_moe` with a smaller `__launch_bounds__` than the block the host
launches, and for Q8_0 the host launched 256 threads into a kernel built for 128. The expert path of
a mixture-of-experts model runs that kernel, so a build without this patch is wrong exactly where
the MoE work is.

The patch also moves gfx1013 onto the `RDNA1` branch of `ggml_cuda_dp4a`, which selects inline
assembly. On a GFX10.1 part with no dot instruction that assembly is an optimisation barrier, not an
optimisation: measured at 2.2 times the instructions and 4.5 times the local-memory reads of the
generic C in a real MMQ kernel (`evidence/m16/gfx1013-dp4a-2026-10-09`). `llama-no-asm-dp4a.patch`
puts that branch behind `BC250_HIP_NO_ASM_DP4A` so the two forms can be measured against each other,
and the release build leaves the macro undefined, which keeps the upstream default.

## Why it is here

Both changes were in the reviewed consumer source snapshot of M16 step 3 from its first day, and the
shipped release asset carries them (its own `patches/source-vs-upstream-b86d2f07.diff` holds the
whole difference). Neither existed in this directory as a patch file, so the patch set here was
incomplete: a rebuild driven from `compute/hip/patches/` alone produced a binary with the
launch-bounds disagreement above and nothing said so. `build-llama-patched.ps1` now checks all four
markers and refuses an incomplete source copy, and `-ApplyPatches` applies the complete set.

The same two findings are drafted for upstream in `scratch/m16-hip/upstream/`
`llama-cpp-gfx1013-rdna1-issue.md`. That draft is not posted. The owner decides whether it goes out.

Nasze gfx1013 bywa niewidzialne w cudzych listach - our gfx1013 tends to be invisible in other
people's lists.
