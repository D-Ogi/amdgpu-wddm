# The dp4a inline assembly is the whole difference: an optimisation barrier in one MMQ kernel

Date: 2026-10-09. Offline measurement on the development machine, no lab unit involved.
Milestone M16, the HIP route of the BC-250 Windows driver.

This directory answers the open question of
[gfx1013-dp4a-2026-10-09](../gfx1013-dp4a-2026-10-09/README.md), section 3. That measurement
compiled one MMQ translation unit for gfx1013 without and with the one-line patch that adds
`__gfx1013__` to llama.cpp's `RDNA1` list, found a whole-kernel difference much larger than one
dot product sequence, and stated plainly that it had not isolated the cause. The words were:
"Something else in this kernel therefore changes with the macro, or the two loop bodies are
unrolled differently."

The first half of that sentence is wrong, and the answer costs one more compile. Nothing else in
that translation unit changes with the macro. The whole difference is the inline assembly of
`ggml_cuda_dp4a`, which is an optimisation barrier.

## The experiment

A third build of the same translation unit, with two edits instead of one:

1. `hip-rdna1-gfx1013.diff` of the earlier directory, so `RDNA1` is defined for gfx1013.
2. `dp4a-asm-off.diff` of this directory, which renames the condition of one branch of
   `ggml_cuda_dp4a` (`ggml/src/ggml-cuda/common.cuh:717`) to a macro that nothing defines.

The generic C byte loop is therefore compiled, while every other reader of `RDNA1` in the file
still sees the macro. The second diff is a measurement instrument, not a proposed change.

## The result

One kernel, `mul_mat_q<GGML_TYPE_Q8_0, 8, false, GGML_PREC_Q8>`, of
`ggml/src/ggml-cuda/template-instances/mmq-instance-q8_0.cu`, target gfx1013. The first two rows
are the earlier directory's own numbers, recompiled here and identical to what it published:

| build | object bytes | instructions | `v_mul_i32_i24_sdwa` | `v_add3_u32` | `ds_read2_b32` | VGPR |
|---|---|---|---|---|---|---|
| without the patch | 599 168 | 1 025 | 232 | 117 | 40 | 200 |
| with the patch | 611 456 | 2 292 | 1 024 | 520 | 180 | 214 |
| with the patch, dp4a asm off | **599 168** | **1 025** | **232** | **117** | **40** | **200** |

The third row is equal to the first in every field of the kernel report: object size, instruction
count, the whole instruction histogram, and every register, LDS, scratch, wavefront and workgroup
figure of the kernel descriptor. The two disassemblies are equal line for line, and the one in
this directory (`mmq/mmq-q8_0-J8.with-patch-no-dp4a-asm.s`) can be compared with
`../gfx1013-dp4a-2026-10-09/mmq/mmq-q8_0-J8.without-patch.s` of the earlier directory, which is
the first row.

## Why the assembly costs this much

The earlier directory's section 2 compiled the two dp4a bodies in isolation and found them equal:
with eight independent dot products in one kernel, both give 32 `v_mul_i32_i24_sdwa` and 16
`v_add3_u32`. That probe is the case where the two forms cannot differ, because its eight calls
share no operand.

An MMQ kernel is the opposite case. It accumulates many dot products over one tile of shared
memory, so the calls do share operands. Out of plain C the compiler sees the byte extraction of
all of them at once, so it shares the work (`v_mad_i32_i24` 24 and `v_lshrrev_b16` 80 appear, and
the shared-memory reads merge into 40 `ds_read2_b32`). An `asm` block is opaque to the compiler:
it cannot know that two blocks read the same bytes, so it cannot common up the extraction and it
cannot merge the reads across them. It emits the four multiplies and two `v_add3_u32` of the
block for every dot product, and it re-reads the tile: 1 024 multiplies and 180 `ds_read2_b32`.
Two more effects follow from the same cause, and both are in the table: 14 more VGPRs, because
the two scratch registers of the block are live per dot product rather than shared, and 12 288
more bytes of object.

So it is 2.2 times the instructions of the kernel, 4.5 times the shared-memory reads and 14 more
registers, for a sequence that the compiler already produces by itself from the C.

## gfx1010, which upstream already names in that list

This is not only about a part that upstream does not know. gfx1010 is
`FeatureISAVersion10_1_0`, which is `FeatureISAVersion10_1_Common` plus an empty list
(`llvm/lib/Target/AMDGPU/AMDGPU.td:1944-1946` of LLVM 23.1.2), so it has no Dot feature either.
gfx1013 differs from it only by `FeatureGFX10_AEncoding`. And `__gfx1010__` is in llama.cpp's
`RDNA1` list already, with no patch at all.

Both builtins are refused for gfx1010 by the same probe, the same compiler and the same flags as
section 1 of the earlier directory (`dot/sdot4.gfx1010.log`, `dot/sudot4.gfx1010.log`):
`__builtin_amdgcn_sdot4` "needs target feature dot1-insts", `__builtin_amdgcn_sudot4` "needs
target feature dot8-insts". The gfx1030 positive control of the earlier directory covers these
two rows as well, because it is the same probe.

The same translation unit, compiled for gfx1010 with the assembly branch and without it:

| build | object bytes | instructions | `v_mul_i32_i24_sdwa` | `ds_read2_b32` | VGPR |
|---|---|---|---|---|---|
| gfx1010, asm branch on (upstream as it stands) | 611 456 | 2 292 | 1 024 | 180 | 214 |
| gfx1010, asm branch off | 599 168 | 1 025 | 232 | 40 | 200 |

Those two kernels are equal line for line to the gfx1013 pair above, which is what the feature
sets predict: this kernel uses nothing that `FeatureGFX10_AEncoding` adds. So gfx1010, a target
upstream supports, pays the same cost today.

## What this is not

It is an instruction-count measurement, and nothing here is a speed claim. Neither form has run
on any hardware, and fewer instructions is not the same thing as faster: the assembly keeps the
dot product on four multiplies and two adds with no byte extraction at all, and on this part
nobody has measured which shape the memory system and the scheduler prefer. What the measurement
does establish is that the earlier open question is closed, that the cost of the patch on this
kernel is removable, and that it is removable without giving up the patch:

- the patch is still needed, and for the reason the earlier directory gives in its section 4: the
  host and the device of one build otherwise disagree about the block size of `mul_mat_vec_q_moe`,
  which is a correctness defect and not a tuning choice.
- the dp4a branch is a separate question from the macro, and on a part with no dot instruction the
  answer that costs least is to leave the dot product to the compiler.

Note for a reader of the earlier directory: the device-side reach of the `RDNA1` macro in this
backend is three places, not the two that directory names. `vendors/hip.h:241` defines `RDNA` from
`RDNA1`, and `RDNA` is read at `fattn-tile.cuh:330`, which selects the whole tile configuration of
flash attention, and at `fattn-vec.cuh:75`, which sets `nthreads_KQ_q` to 2 instead of 4. For this
part both of those values are the correct ones, so that effect of the patch is favourable. The MMQ
translation unit measured here is built with `-DGGML_CUDA_NO_FA=1`, so neither of the two is in it.

## How to reproduce

`<llvm>` is the portable AMDGPU clang's `bin` directory, `<llama>` a checkout of llama.cpp at
`b86d2f075`, and `<root>` the ROCm-shaped root that `compute/hip/tools/make-rocm-root.py`
assembles out of this repository's `compute/hip/include` and the device library bitcode. The
compiler is `clang version 22.1.8`. The full banner is in `clang-version.txt`.

**Run this from a `vcvars64` shell.** The command names a Windows host triple, so the host pass of
this translation unit includes `<cmath>` and needs the MSVC and Windows SDK include set on
`INCLUDE`. Without it the compile fails before it reaches any device code. The earlier directory's
reproduce section does not say so, and that is the one thing it is missing. `evidence/` is
immutable, so the sentence is here instead.

Three builds of one command. Build 1 is the checkout as upstream has it. Build 2 applies
`../gfx1013-dp4a-2026-10-09/hip-rdna1-gfx1013.diff`. Build 3 applies that diff and then
`dp4a-asm-off.diff`:

    <llvm>/clang++ -x hip --target=x86_64-pc-windows-msvc --offload-arch=gfx1013 \
        --offload-device-only --no-gpu-bundle-output -O2 -std=c++17 -fno-exceptions \
        --rocm-path=<root> -include __clang_hip_runtime_wrapper.h \
        -D__HIP_PLATFORM_AMD__=1 -DGGML_USE_HIP=1 -DGGML_HIP_NO_VMM=1 -DGGML_CUDA_NO_FA=1 \
        -DGGML_CUDA_FORCE_MMQ=1 -DGGML_HIP_NO_MMQ_MFMA=1 -DNDEBUG \
        -I <root>/include -I <llama>/ggml/include -I <llama>/ggml/src \
        -c -o mmq.o <llama>/ggml/src/ggml-cuda/template-instances/mmq-instance-q8_0.cu

The gfx1010 pair is the same command with `--offload-arch=gfx1010`, and without the first diff,
because `__gfx1010__` is in the `RDNA1` list already. The two gfx1010 dot-instruction logs come
from the earlier directory's own section 1 command with `--offload-arch=gfx1010`.

The per-kernel figures of both tables are the instruction histogram of one kernel's own
disassembly range plus the fields of its kernel descriptor, from `llvm-objdump -d` and
`llvm-readelf --notes` of the object. The mangled kernel name is
`_ZL9mul_mat_qIL9ggml_type8ELi8ELb0EL9ggml_prec30EEvPKcPKiS5_S5_PfS6_PKf5uint3iiiiiS9_S9_iiiS9_S9_iiiS9_`.
The `.json` files of `mmq/` hold the whole histogram and descriptor of each build, so any row of
either table can be checked without the compiler.

One convention, stated because two counts of the same kernel can differ by one: the instruction
counts above come from the histogram of the kernel's disassembly range, which is 1 025 for the
first build. A counter that also counts the trailing `s_code_end` padding of the range gives
1 026 for the same object. Both tables use the first convention throughout, and so does the
earlier directory.
