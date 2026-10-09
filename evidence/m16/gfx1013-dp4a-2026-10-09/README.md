# gfx1013, the 8-bit dot product, and llama.cpp's RDNA1 macro

Date: 2026-10-09. Offline measurement on the development machine, no lab unit involved.
Milestone M16, the HIP route of the BC-250 Windows driver.

## What this directory answers

llama.cpp's HIP backend chooses how to compute a 4-byte integer dot product from the target it is
compiled for. Three questions decide which branch a gfx1013 part takes, and all three are measured
here:

1. Does gfx1013 have `v_dot4_i32_i8` (`__builtin_amdgcn_sdot4`) or `v_dot4_i32_iu8`
   (`__builtin_amdgcn_sudot4`)? **Neither.**
2. ggml-cuda's `RDNA1` branch uses inline assembly instead. Is that assembly better than the
   generic C fallback on this part with a current compiler? **No: with more than one dot product
   to schedule the two compile to the same instructions.**
3. Does it matter whether gfx1013 is named in that `RDNA1` list at all? **Yes, and not for the
   dot product: the host and the device of one build otherwise disagree about the block size of
   one kernel.**

## The material

- llama.cpp: commit `b86d2f075` ("cuda: FWHT kernels for block widths above 512 (#29100)").
- Compiler: `clang version 22.1.8`, the portable AMDGPU build. The full banner is in
  `clang-version.txt`. The target of every device compilation is `gfx1013`. `gfx1030` appears
  only as the positive control of question 1.
- LLVM target definitions read for the same question: `llvm/lib/Target/AMDGPU/AMDGPU.td`
  (`FeatureISAVersion10_1_3`) and `llvm/lib/Target/AMDGPU/GCNProcessors.td` (the `gfx1013`
  processor), LLVM 23.1.2 and 19.1.7.

## 1. The dot instructions: `dot/`

Each log holds its own command line, its own output and its exit status.
`dot/probe-dot.hip` is the whole source: one kernel per builtin, nothing else.

| builtin | gfx1013 | gfx1030 (control) |
|---|---|---|
| `__builtin_amdgcn_sdot4` | refused: "needs target feature dot1-insts" | accepted, becomes `v_dot4c_i32_i8` |
| `__builtin_amdgcn_sudot4` | refused: "needs target feature dot8-insts" | refused: "needs target feature dot8-insts" |

The control is what makes the first row mean something: the same probe, the same compiler and the
same flags do produce the instruction on a part that has the feature. `dot8-insts` is a GFX11
feature, so the second row is expected on both.

Consequence for llama.cpp: of the four branches of `ggml_cuda_dp4a`
(`ggml/src/ggml-cuda/common.cuh:711-734`), the two that use a builtin are closed on gfx1013. What
is left is the `RDNA1` inline assembly and the generic C byte loop.

## 2. The two remaining branches, compiled in isolation: `dp4a/`

`dp4a/probe-dp4a.hip` holds both bodies, copied unchanged out of `ggml_cuda_dp4a`, in one kernel
with one call and in one kernel with eight independent calls. Every operand is indexed by the lane,
because with operands that are equal in every lane the compiler puts the generic form on the
scalar unit, which is not the code a matrix multiply kernel gets.

One call, the vector instructions of the whole kernel:

| | generic C byte loop | RDNA1 inline assembly |
|---|---|---|
| `v_mul_i32_i24_sdwa` | 2 | 4 |
| `v_mad_i32_i24` | 2 | 0 |
| `v_add3_u32` | 1 | 2 |
| `v_bfe_i32` | 4 | 0 |
| `v_lshrrev_b16` | 2 | 0 |
| `v_lshlrev_b32` (the lane's address) | 1 | 1 |
| vector instructions in all | **12** | **7** |

Eight independent calls, the same counts:

| | generic C byte loop | RDNA1 inline assembly |
|---|---|---|
| `v_mul_i32_i24_sdwa` | 32 | 32 |
| `v_add3_u32` | 16 | 16 |
| `v_lshlrev_b32` (the lane's address) | 1 | 1 |
| vector instructions in all | **49** | **49** |

So the compiler reaches the inline assembly's own instruction sequence by itself, out of plain C,
as soon as there is more than one dot product in flight: `v_mul_i32_i24` with SDWA byte selectors
and `v_add3_u32`, four and two per dot product, in both. The single-call kernel is the case where
the two differ, and there the generic form pays five extra instructions for byte extraction that
it has nothing to amortise over. The eight-call shape is the shape a matrix multiply has. One
vector instruction in each count belongs to the kernel and not to the dot product: the shift that
turns the lane index into a byte offset.

This is a statement about instruction selection by this compiler for this target. It is not a
statement about speed: nobody has run either form on the hardware yet. The two disassemblies are
`dp4a/dp4a-generic-c.gfx1013.s` and `dp4a/dp4a-rdna1-asm.gfx1013.s`, both kernels in each.

## 3. The same MMQ kernel, built both ways: `mmq/`

The patch under measurement is `hip-rdna1-gfx1013.diff`: it adds `__gfx1013__` to the `RDNA1` list
in `ggml/src/ggml-cuda/vendors/hip.h`. One translation unit,
`ggml/src/ggml-cuda/template-instances/mmq-instance-q8_0.cu`, was compiled for gfx1013 without and
with it, by the same command. One kernel of the result,
`mul_mat_q<GGML_TYPE_Q8_0, 8, false, GGML_PREC_Q8>`, is reported here. Its two disassemblies and
the two full instruction histograms are the `.s` and `.json` files.

| | without the patch | with the patch |
|---|---|---|
| object, whole translation unit | 599 168 bytes | 611 456 bytes |
| instructions in this kernel | 1 025 | 2 292 |
| `v_mul_i32_i24_sdwa` | 232 | 1 024 |
| `v_add3_u32` | 117 | 520 |
| `v_mad_i32_i24` | 24 | 0 |
| `v_lshrrev_b16` | 80 | 0 |
| `ds_read2_b32` | 40 | 180 |
| VGPR | 200 | 214 |
| SGPR | 32 | 32 |
| LDS, static | 0 bytes | 0 bytes |
| scratch | 0 bytes | 0 bytes |
| wavefront size | 32 | 32 |
| maximum workgroup size | 256 | 256 |

Both builds are wave32, and neither spills.

**An open question, stated as one.** These two kernels differ by more than the dot product
sequence: the shared-memory reads differ as well (40 against 180), and section 2 shows that the two
dot product forms compile to the same instructions in this shape. Something else in this kernel
therefore changes with the macro, or the two loop bodies are unrolled differently. This measurement
did not isolate which of the two it is. The table is whole-kernel data, and no claim is made here that the patch
makes this kernel slower or faster. The device-side reads of `RDNA1` inside this backend are only
two (`common.cuh:717` and `mmvq.cu:437`), and the MMQ tile configuration falls to the same
`rdna2` table either way (`mmq.cuh:265-281`), so the cause is not an obvious table swap.

## 4. Why the patch is still needed: the host and the device disagree

This part needs no measurement beyond reading the backend, and it is the reason the patch exists.

- The host maps a device to a compute capability from the `gcnArchName` string
  (`ggml-cuda.cu:172-218`). "gfx1013" becomes `GGML_CUDA_CC_OFFSET_AMD + 0x1013`, and
  `GGML_CUDA_CC_IS_RDNA1` is `>= 0x1010 && < 0x1030` (`common.cuh:80-88`). So the host already
  calls gfx1013 an RDNA1 part, with no patch at all.
- The device constexpr `get_mmvq_mmid_max_batch_for_device` (`mmvq.cu:430-450`) reads the `RDNA1`
  macro. Without the patch that macro is absent and the constexpr falls through to
  `get_mmvq_mmid_max_batch_pascal_older`.
- That constexpr is the `__launch_bounds__` of `mul_mat_vec_q_moe` (`mmvq.cu:858`), multiplied by
  the warp size. The host launches that kernel with a block of `(warp_size, ncols_dst)`
  (`mmvq.cu:1067`), where `ncols_dst` is bounded by the host's own table
  (`get_mmvq_mmid_max_batch`, `mmvq.cu:285-315`).

The two tables do not agree. `MMVQ_MAX_BATCH_SIZE` is 8 (`mmvq.cuh:3`), and the warp size of this
part is 32:

| type | host bound (RDNA1 table) | device bound without the patch | block launched | block compiled for |
|---|---|---|---|---|
| Q8_0 | 8 | 4 | 256 threads | 128 threads |
| Q4_0 | 8 | 6 | 256 threads | 192 threads |
| Q2_K | 7 | 4 | 224 threads | 128 threads |
| Q6_K | 5 | 4 | 160 threads | 128 threads |

A block larger than the kernel's `amdgpu_flat_work_group_size` is not a slow launch, it is an
invalid one. This only reaches a mixture-of-experts model, because `mul_mat_vec_q_moe` is the
`mul_mat_id` path, but within that path it is a correctness defect and not a tuning choice.
The warp size itself does not depend on the macro: `ggml_cuda_get_physical_warp_size`
(`common.cuh:390-396`) answers 32 for everything that is not GFX9 or GFX8.

`gfx1011` is in the same position: it is also absent from that `RDNA1` list and also inside the
host's RDNA1 range. This directory did not test a gfx1011 part, so that is a question for upstream
and not a claim here. Note that gfx1011 does have `dot1-insts` (`AMDGPU.td`), so the right branch
for it may be the `sdot4` one rather than the assembly one.

## How to reproduce

The compiler is the only tool needed. `<llvm>` is the portable AMDGPU clang's `bin` directory and
`<llama>` is a checkout of llama.cpp at `b86d2f075`.

Section 1, for each of `gfx1013` and `gfx1030` and each of `BC250_PROBE=1` (sdot4) and `2`
(sudot4):

    <llvm>/clang++ -x hip --offload-arch=<arch> --offload-device-only --no-gpu-bundle-output \
        -nogpuinc -nogpulib -O2 -std=c++17 -DBC250_PROBE=<n> -c -o probe.o dot/probe-dot.hip

Section 2, for each of `BC250_DP4A=1` (generic C) and `2` (RDNA1 assembly):

    <llvm>/clang++ -x hip --offload-arch=gfx1013 --offload-device-only --no-gpu-bundle-output \
        -nogpuinc -nogpulib -O2 -std=c++17 -DBC250_DP4A=<n> -c -o dp4a.o dp4a/probe-dp4a.hip
    <llvm>/llvm-objdump -d dp4a.o

Section 3 needs a HIP header set, because the translation unit is a real one. Ours is in this
repository under `compute/hip/include`, and the device library bitcode is built from AMD's ROCm
device library sources. `<root>` below is the ROCm-shaped root that
`compute/hip/tools/make-rocm-root.py` assembles out of the two. The patched build is the same
command after applying `hip-rdna1-gfx1013.diff` to the checkout.

    <llvm>/clang++ -x hip --target=x86_64-pc-windows-msvc --offload-arch=gfx1013 \
        --offload-device-only --no-gpu-bundle-output -O2 -std=c++17 -fno-exceptions \
        --rocm-path=<root> -include __clang_hip_runtime_wrapper.h \
        -D__HIP_PLATFORM_AMD__=1 -DGGML_USE_HIP=1 -DGGML_HIP_NO_VMM=1 -DGGML_CUDA_NO_FA=1 \
        -DGGML_CUDA_FORCE_MMQ=1 -DGGML_HIP_NO_MMQ_MFMA=1 -DNDEBUG \
        -I <root>/include -I <llama>/ggml/include -I <llama>/ggml/src \
        -c -o mmq.o <llama>/ggml/src/ggml-cuda/template-instances/mmq-instance-q8_0.cu
    <llvm>/llvm-objdump -d mmq.o
    <llvm>/llvm-readelf --notes mmq.o

The kernel figures in the table come from the `--notes` metadata of that object and from the
instruction histogram of that one kernel's disassembly range.
