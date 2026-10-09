# compute/hipblas - the BLAS that llama.cpp's HIP backend asks for

Milestone M16, route B, step 3. llama.cpp's `ggml-hip` backend requires three CMake packages and
links against one of them. We have a HIP runtime of our own (`compute/hip`), and we have no
hipBLAS and no rocBLAS. This directory holds the decision about that, the measurement behind it,
and what is still missing.

Nothing here is built yet. The header and the two CMake packages live in `compute/hip`, because
they belong to the include tree and to the ROCm-shaped root that
`compute/hip/tools/make-rocm-root.py` assembles.

## What the backend really needs

MEASURED 2026-10-09 with `scratch\m16-hip\step3\probe\inventory.py` over llama.cpp
`b86d2f07542b29ab099aed34fd6b6d1b2fd4b81c`, and confirmed by the link of `ggml-hip.dll`.

The inventory finds 95 distinct host-side calls in the backend. Eleven of them are BLAS names, and
the link asks for ten. These:

| Name | Who calls it | What it must do |
|---|---|---|
| `hipblasCreate`, `hipblasDestroy` | one handle per device, in `ggml_backend_cuda_context` | a handle |
| `hipblasSetStream` | before every call | bind the handle to a stream |
| `hipblasGemmEx` | `ggml_cuda_mul_mat` with f16 input | one mixed-precision GEMM |
| `hipblasGemmBatchedEx`, `hipblasGemmStridedBatchedEx` | batched matrix multiply, and attention when FlashAttention is off | the same, batched |
| `hipblasSgemm`, `hipblasSgemmBatched`, `hipblasSgemmStridedBatched` | the f32 paths | one f32 GEMM |
| `hipblasStrsmBatched` | `solve_tri.cu:62` only | a triangular solve |

The eleventh name of the inventory is not reached by the link, and `rocblas` is required by the
CMake file and never called at all.

## The route chosen, and why

Two routes were possible. The smaller one won.

**(a) Patch the backend to make BLAS optional.** It would need changes in four files of a project
we do not own, and every pull of llama.cpp would have to carry them again. The backend's
`CMakeLists.txt` asks for `hipblas` and `rocblas` as `REQUIRED`, and `vendors/hip.h` includes
`hipblas/hipblas.h` unconditionally, so making it optional is not one line.

**(b) Answer with packages and a header of our own.** `find_package(hip)`, `find_package(hipblas)`
and `find_package(rocblas)` all succeed against `compute/hip/cmake`, the include of
`hipblas/hipblas.h` resolves, and llama.cpp stays unpatched. For `rocblas` the package is honestly
empty: it defines the target the CMake file expects, says in a comment that nothing calls into it,
and names no import library.

Route (b) is what is built. The cost of it is this directory: the ten names need an
implementation, and ours will not be a BLAS library. It will be the smallest thing that answers
those ten calls correctly.

## The smallest next step

A `bc250hipblas.dll` with:

- a tiled f32 GEMM on our own HIP runtime, behind `hipblasSgemm` and its batched and
  strided-batched forms.
- `hipblasGemmEx` and its two batched forms over the same kernel, with the f16 input converted on
  the way in, because `HIPBLAS_R_16F` is the type llama.cpp passes for a quantized model's
  dequantized weights.
- `hipblasStrsmBatched` answering `HIPBLAS_STATUS_NOT_SUPPORTED`. It serves ggml's `solve_tri`
  operation only, which inference never reaches. A refusal there is honest and a wrong triangular
  solve is not.

One trap for that work, measured during the step-3 build: with `GGML_CUDA_NO_FA=1` the attention
matrix multiplies go through `hipblasGemmStridedBatchedEx`, so this GEMM is on the hot path of
every token and must really compute. With FlashAttention on, attention goes to ggml's own kernels
and the GEMM carries the weight matrices only.

The alternative to writing a GEMM is to dequantize and route the matrix multiply through ggml's
own MMQ kernels, which the backend already has and which `GGML_CUDA_FORCE_MMQ=ON` prefers. That
does not remove the ten names: the link needs them whether they are called or not. It only lowers
how good they have to be.

## What is where

| Path | What it is |
|---|---|
| `compute/hip/include/hipblas/hipblas.h` | the interface: the handle, the five enumerations, the ten entry points. Written against the calls the backend makes, not against the whole of hipBLAS |
| `compute/hip/cmake/hipblas-config.cmake` | the package. Defines `roc::hipblas` and `hip::hipblas`, and states plainly when no import library is present |
| `compute/hip/cmake/rocblas-config.cmake` | the empty honest package for the name the CMake file requires and nobody calls |

`hipblasStrsmBatched` takes `const float *const A[]`, which is what the call site in `solve_tri.cu`
passes. The first version of the header took `const float **` and did not compile.

Jedna jaskółka wiosny nie czyni - one swallow does not make a spring: three CMake packages and a
header are not a BLAS. They are what lets the rest of step 3 be measured.
