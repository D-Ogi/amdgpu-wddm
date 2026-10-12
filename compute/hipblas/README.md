# compute/hipblas - the BLAS that llama.cpp's HIP backend asks for

Milestone M16, route B, step 3. llama.cpp's `ggml-hip` backend requires three CMake packages and
links against one of them. We have a HIP runtime of our own (`compute/hip`), and we have no
hipBLAS and no rocBLAS. This directory holds the decision about that, the measurement behind it,
and the library that answers the ten names.

## What the backend really needs

MEASURED 2026-10-09 with `scratch\m16-hip\step3\probe\inventory.py` over llama.cpp
`b86d2f07542b29ab099aed34fd6b6d1b2fd4b81c`, and the link of `ggml-hip.dll` agrees.

The inventory finds 95 distinct host-side calls in the backend. Eleven of them are BLAS names, and
the link asks for ten. These:

| Name | Who calls it | What it does here |
|---|---|---|
| `hipblasCreate`, `hipblasDestroy` | one handle per device, in `ggml_backend_cuda_context` | a handle, whose whole state is one stream |
| `hipblasSetStream` | before every call | bind the handle to a stream |
| `hipblasGemmEx` | `ggml_cuda_mul_mat` with f16 or bf16 input, `conv2d.cu:414`, `conv3d.cu:330` | one typed GEMM |
| `hipblasGemmBatchedEx`, `hipblasGemmStridedBatchedEx` | batched matrix multiply, and attention when FlashAttention is off | the same, batched |
| `hipblasSgemm`, `hipblasSgemmBatched`, `hipblasSgemmStridedBatched` | the f32 paths, and every call of `out-prod.cu` | one f32 GEMM |
| `hipblasStrsmBatched` | `solve_tri.cu:70` only | **refused** with `HIPBLAS_STATUS_NOT_SUPPORTED` |

The eleventh name of the inventory is not reached by the link, and `rocblas` is required by the
CMake file and never called at all.

## The route chosen, and why

Two routes were possible. The smaller one won.

**(a) Patch the backend to make BLAS optional.** It would need changes in four files of a project
we do not own, and every pull of llama.cpp would have to carry them again. The backend's
`CMakeLists.txt` asks for `hipblas` and `rocblas` as `REQUIRED`, and `vendors/hip.h` includes
`hipblas/hipblas.h` unconditionally, so making it optional is not one line.

**(b) Answer with packages, a header and a library of our own.** `find_package(hip)`,
`find_package(hipblas)` and `find_package(rocblas)` all succeed against `compute/hip/cmake`, the
include of `hipblas/hipblas.h` resolves, `bc250hipblas.lib` answers the link, and llama.cpp stays
unpatched. For `rocblas` the package is honestly empty: it defines the target the CMake file
expects, says in a comment that nothing calls into it, and names no import library.

Route (b) is what is built.

## What is built

`bc250hipblas.dll`, one translation unit, 11 exported names, HIP device code for gfx1013 over our
own HIP runtime. It is not a BLAS and it does not pretend to be one: it implements the element
types and the two operations that one caller passes, and refuses everything else.

| Path | What it is |
|---|---|
| `src/gemm_core.h` | the matrix multiply itself: the type table, the argument rules, the column-major index arithmetic, the three tile phases and an independent reference. Written once for three readers - the kernel, the host test and the device test |
| `src/bc250hipblas.hip` | the two kernels and the eleven entry points |
| `bc250hipblas.def` | the export list. `build-hipblas.ps1` compares it with the built DLL |
| `tests/host/test_gemm_core.cpp` | the host test: the argument rules, and the kernel's own tile phases run serially on the CPU against the reference |
| `tests/gemm_check.hip` | the device test program: the refusals on any machine, the numbers on the hardware |
| `build-hipblas.ps1` | the build, the two tests and the mock run |
| `compute/hip/include/hipblas/hipblas.h` | the interface: the handle, the five enumerations, the eleven entry points. Written against the calls the backend makes, not against the whole of hipBLAS |
| `compute/hip/cmake/hipblas-config.cmake` | the package. Defines `roc::hipblas` and `hip::hipblas` and finds `bc250hipblas.lib` |
| `compute/hip/cmake/rocblas-config.cmake` | the empty honest package for the name the CMake file requires and nobody calls |

### The kernel

Column major, as hipBLAS is. C is `m x n` with leading dimension `ldc`. A is `m x k` when
`transA` is N and `k x m` when it is T. B is `k x n` when `transB` is N and `n x k` when it is T.
`lda`, `ldb` and `ldc` are honoured, `alpha` and `beta` are honoured, and **`beta == 0` does not
read C**, which is not an optimisation: ggml writes into a pool allocation that holds the bytes of
an earlier tensor, so a read would turn a stale NaN into a NaN result.

One workgroup of 16 x 16 work items computes one 16 x 16 tile of C and walks the reduction in
steps of 16 through two tiles in local memory: 256 work items, eight wave32 waves, 2 KB of local
memory, one output per work item. An index outside a matrix reads as zero, which is what makes an
odd shape correct with no second kernel for the boundary.

It is the correctness-first shape on purpose. This library exists so that `ggml-hip` links and
runs, and with `GGML_CUDA_FORCE_MMQ=ON` every quantized matrix multiply of a model goes to ggml's
own MMQ kernels instead. **Open work, for after the lab measures it.** The first item is a 2x2
micro-tile per work item over a 32 x 32 tile, which quarters the local-memory traffic per
multiply. The second is the f32 path of attention. With `GGML_CUDA_FA=OFF` that path goes through
`hipblasGemmStridedBatchedEx` for every token, so it is the one call where the tile shape shows.

Two kernels, not one: the batched forms take arrays of pointers **in device memory**, because ggml
fills those arrays with a kernel of its own (`ggml-cuda.cu:1588`, `out-prod.cu:100`), so the host
cannot read them. The strided form takes a base pointer and an element stride per batch element.

### The type table, and why a refusal is the safe answer

`accept_types()` in `src/gemm_core.h` is the whole table:

| aType, bType | cType | computeType | Who passes it |
|---|---|---|---|
| `HIPBLAS_R_32F` | `R_32F` | `R_32F` | `ggml_cuda_mul_mat_cublas_impl<F32>`, every `hipblasSgemm*` of `out-prod.cu` |
| `HIPBLAS_R_16F` | `R_16F` | `R_16F` | the f16 path on this part: `prefer_f32_output` is false for RDNA1 (`ggml-cuda.cu:1506-1512`) |
| `HIPBLAS_R_16F` | `R_32F` | `R_32F` | `conv2d.cu:414`, `conv3d.cu:330` |
| `HIPBLAS_R_16B` | `R_32F` | `R_32F` | the bf16 path, where `prefer_f32_output` is true on this part |
| `HIPBLAS_R_16F` | `R_16F` | `R_32F` | nobody today. It is accepted because it is the same work |

Everything else answers `HIPBLAS_STATUS_NOT_SUPPORTED`, and `HIPBLAS_OP_C` does the same. The
reason this matters more than it looks: with `GGML_CUDA_FORCE_MMQ=ON` a quantized type that MMQ
does not cover still reaches `hipblasGemmEx` through `ggml_cuda_mul_mat_cublas`, so an unknown
`hipblasDatatype_t` read as f32 would be a wrong number in the model's output with no message,
while a refusal is a stated abort through ggml's own `CUBLAS_CHECK`.

The accumulator is f32 for every accepted combination, including the one whose compute type is
`R_16F`. That is more accurate than the letter of the type and never less, and it is what the
hardware wants: gfx1013 has no f16 dot instruction. The result is rounded to f16 one time, on the
store. `alpha` and `beta` are read in the compute type, as hipBLAS states, so a call with
`R_16F` as its compute type passes two `__half` - which `ggml-cuda.cu:1399-1402` does.

### What is tested, where, and what is not

The mock backend of layer 2 records a dispatch and executes no instruction, so no kernel can run
on the development PC. The tests are split along that line and each one says which side it is on.

| Test | Machine | What it proves |
|---|---|---|
| `test_gemm_core.exe` | development PC | **550 checks, 0 failures.** It covers the type table and every refusal in it, the argument rules, and the index arithmetic. Then it runs the kernel's own `tile_load`, `tile_mac` and `tile_store` serially over every work item of every workgroup, against `reference_gemm`. The cases are 8 shapes by 4 transpositions by 2 betas in f32, and three element combinations. The batch counts are 1, 3 and 5, in both batch modes. `k == 0` is one of the cases. Each case has a negative control that moves one element of A and must be noticed. One more case: `beta == 0` over a destination full of NaN comes back finite |
| `gemm_check.exe --no-numeric` | development PC, against the mock | **446 checks, 0 failures, 43 dispatches.** It covers every entry point through its own interface and every refusal. It also shows that a refused call builds no dispatch. The program counts the dispatches it expects, and the build script compares that number with the mock's record. Every dispatch carries the 16 x 16 workgroup |
| `gemm_check.exe` | unit A | the numbers on the hardware. Lab arm (a) of `scratch\m16-hip\lab\step3-README.md` part 3B |

What the host test cannot prove: the real barrier, the real local memory, the dispatch and the
hardware. That is the device test, and until it has run on unit A this library is correct on the
CPU and unproven on the GPU. Jedna jaskółka wiosny nie czyni - one swallow does not make a
spring.

### What the whole stack does with it

MEASURED 2026-10-09. With this library and the mock build of `amdhip64.dll`, `ggml-hip.dll`
links, and `llama-completion.exe` and `llama-bench.exe` start, answer `--version` and enumerate
the device:

```
Device 0: AMD BC-250 (mock device), gfx1013 (0x1013), VMM: no, Wave Size: 32, VRAM: 8192 MiB
```

A model does not run there, and the reason is not this library. The mock backend is a test double
with static tables (16 kernels, 128 symbols, 96-character names per module), and ggml's
code objects hold
up to 352 kernels and 2476 symbols in one translation unit. Design section 4.14 has the numbers.
The first inference of this route is a lab arm.

## How to build it

```
pwsh bc250-win\compute\hip\build.ps1 -Out P:\bc-250\scratch\build\m16-step3-l1
pwsh bc250-win\compute\hip\build-runtime.ps1 -Bc250hsaLib P:\bc-250\scratch\build\m16-step3-l1\bc250hsa.lib -Out P:\bc-250\scratch\build\m16-step3
pwsh bc250-win\compute\hipblas\build-hipblas.ps1 -HipLib P:\bc-250\scratch\build\m16-step3
```

The whole of this component is built by the portable AMDGPU clang
(`toolchain\llvm-amdgpu-22.1.8`), not by MSVC: it is HIP device code, which MSVC cannot compile.
MSVC is still needed for its environment, because clang compiles the host pass of a `.hip` file as
well and that pass includes `<cmath>`. The script takes that environment from `vcvars64`, as
`build-runtime.ps1` does.

Then assemble the ROCm-shaped root and build llama.cpp against it:

```
python bc250-win\compute\hip\tools\make-rocm-root.py --out <root> --lib <dir with both .lib files> --bitcode <device library build>
pwsh scratch\m16-hip\step3\tools\build-llama-hip.ps1 -Configure
```
