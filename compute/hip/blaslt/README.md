# The hipBLASLt front

`libhipblaslt.dll` of this directory replaces AMD's `libhipblaslt.dll` in a TheRock Python environment (a venv with
`rocm_sdk_core`, `rocm_sdk_libraries` and torch). It forwards every call to AMD's library and serves one case that
AMD's library cannot serve on this device.

## Why

TheRock's hipBLASLt for gfx1010 has no kernel library ("Cannot read TensileLibrary_lazy_gfx1010.dat"). Each
`hipblasLtMatmulAlgoGetHeuristic` returns `HIPBLAS_STATUS_INVALID_VALUE` with no algorithm. `torch._int_mm`
(i8 x i8 -> i32) has no route other than hipBLASLt (aten `CUDABlas.cpp`, `int8_gemm`). ComfyUI 0.42.0 calls
`torch._int_mm` on every AMD GPU through comfy-kitchen, and no command-line option of ComfyUI changes that. The
design rule is that ComfyUI runs unchanged, so the fix goes into our stack. rocBLAS of the same wheel has gfx1010
kernels for i8 x i8 -> i32 (the Tensile `I8II_BH` kernels).

## What it does

Forward by default. Each export of AMD's DLL is a PE forwarder to `libhipblaslt_amd.dll`, at AMD's ordinal, data
exports included. `build-blaslt-front.ps1` writes the `.def` file from AMD's export table, so the table of the
front is always the table of the AMD DLL that it was built for. Nine names are code of `hipblaslt_front.cpp`.
Each of them calls AMD's function first:

| Name | What the front adds |
|---|---|
| `hipblasLtMatmulDescCreate`, `...Destroy`, `...SetAttribute` | A record of the descriptor: compute type, scale type, TRANSA, TRANSB, and the attributes set away from their defaults |
| `hipblasLtMatrixLayoutCreate`, `...Destroy`, `...SetAttribute` | A record of the layout: type, rows, cols, ld, order, batch count, and other attributes set |
| `hipblasLtMatmulAlgoGetHeuristic` | The served case: AMD's answer if AMD gives an algorithm, else one algorithm of the front |
| `hipblasLtMatmul` | An algorithm of the front: `rocblas_gemm_ex` on the stream of the caller |
| `hipblasLtDestroy` | Destroys the rocBLAS handle that the front made for the hipBLASLt handle |

The front keeps its own records because AMD's library does not report the problem back. MEASURED on hipBLASLt
1.5.0 (rocm-libraries `db2d71e0`): `hipblasLtMatrixLayoutGetAttribute` refuses TYPE, ORDER, ROWS, COLS and LD,
and `hipblasLtMatmulDescGetAttribute` refuses the C and D scale pointers and the A and B scale modes
(`scratch/t36/f11-rocm/probe/t33d-attr.txt`). A descriptor or layout without a record is not a served case.

## The served cases

This list is complete. Every other problem goes to AMD's library, and the caller gets AMD's answer.

- **I8I32.** The descriptor has compute type `HIPBLAS_COMPUTE_32I` and scale type `HIP_R_32I`. TRANSA and TRANSB
  are each N or T. Every other descriptor attribute is at its default or never set: epilogue DEFAULT, pointer
  mode HOST, no bias, scale, amax, aux or fused-epilogue pointer, scale modes SCALAR_32F, SM count target 0.
  A and B are `HIP_R_8I`, and C and D are `HIP_R_32I`. All four layouts are column order with batch count 1.
  No layout attribute is set other than type, order, rows, cols, ld, batch count and strided batch offset. The
  sizes fit int32, each ld is at least its rows, and the sizes agree with each other.

Real first. For a served case, the front asks AMD's heuristic once per transpose pair and process. If AMD returns
an algorithm, AMD's answer stands and the matmul goes to AMD. If AMD refuses, the front returns one algorithm with
the marker `BC250I8` in `hipblasLtMatmulAlgo_t.data` and no workspace. `hipblasLtMatmul` with that algorithm runs
`rocblas_gemm_ex(i8, i8, i32, i32, compute i32, standard, flags 0)`. A marked algorithm with a problem outside
the served case gets `HIPBLAS_STATUS_INVALID_VALUE`, and AMD's library never sees it.

rocBLAS imports hipBLASLt itself. While the front is in a rocBLAS call on a thread, the wrapped names of that thread
only forward. Thus rocBLAS gets AMD's answers, and no call returns to the front.

Switches: `BC250_BLASLT_FRONT=0` turns serving off, and every call forwards. `BC250_BLASLT_LOG=1` writes one line to
the error stream at AMD's first refusal per transpose pair and at each refused or failed served matmul.
`bc250lt_get_counters` (version 2) reads the counters of the front.

## The attach step

The front is a component of our ROCm stack. It installs with our HIP runtime in the same way as the AMD ABI face
(`amdhip64_7.dll`, `build-amd-abi.ps1`), through one step: **`attach-rocm-venv.ps1`**.

1. `make-rocm-payload.ps1` writes the payload: our `amdhip64_7.dll`, our `libhipblaslt.dll`,
   `attach-rocm-venv.ps1`, and `payload.json`. `payload.json` names the TheRock build and four SHA-256 values:
   our two DLLs, and the two AMD DLLs that they replace and were tested against.
2. The release package carries the payload as `payload\tools\hip\rocm`. The installer copies `payload\tools\hip`
   to `<install root>\tools\hip`, so the payload arrives with the HIP runtime. `tools\hip\rocm` is not on PATH,
   so no process outside an attached venv loads these two DLLs. The rows in `tools/release/release-sources.json`
   are still to be added.
3. A user runs `<install root>\tools\hip\rocm\attach-rocm-venv.ps1 -Venv <venv>` once per venv, because each
   venv has its own copy of TheRock's DLLs:
   - `_rocm_sdk_core\bin\amdhip64_7.dll` becomes ours, and AMD's DLL is kept as `amdhip64_7.dll.orig`. It is
     never loaded (D015).
   - `_rocm_sdk_libraries\bin\libhipblaslt.dll` becomes the front, and AMD's DLL is kept as
     `libhipblaslt_amd.dll` in the same directory. AMD's kpack device code still resolves from that directory.
   - `<venv>\bc250-rocm-attach.json` records the four hashes.
4. `-Check` reports the state and changes nothing. `-Detach` puts both AMD DLLs back. `-PlanOnly` reports what the
   step would do.

The step refuses a venv whose AMD DLL is not the one that `payload.json` names, for example another TheRock nightly.
The export table of the front is the table of one AMD build. A refusal changes no file. A file in use (a running
Python of the venv) stops the step before its first change. pip knows only the names in the wheel's RECORD:

| After | State | What to do |
|---|---|---|
| `pip install --force-reinstall` of the same wheel | `reinstalled` (AMD's DLL live, AMD's kept copy beside it) | Attach again |
| A wheel of another TheRock build | `other` | Install the build of `payload.json`, then attach |

Not done yet:
- The installer rows.
- A way to attach from the control app.
- A list of attached venvs for the uninstaller. Today, after an uninstall, an attached venv keeps our face and
  cannot open the device. `-Detach` before the uninstall prevents that.

## Tests

- `tests/host/test_blaslt_front.cpp` (run by `build-blaslt-front.ps1`): the front between a stand-in for AMD's
  library and a CPU `rocblas.dll`.
  - The stand-in refuses GetAttribute as hipBLASLt 1.5.0 does.
  - The numbers of i8 x i8 -> i32 for eight shapes, with each transpose pair and with padding, are checked against
    a CPU reference. So are alpha 3 and beta 2 with a separate C.
  - Real first, and the arguments of `rocblas_gemm_ex`.
  - Fifteen problems outside the served case.
  - Attributes set back to their defaults, the guard against calls from inside rocBLAS, the counters, and
    `BC250_BLASLT_FRONT=0`.
- `tests/host/test-attach-rocm-venv.ps1`: the attach step on a stand-in venv, in Windows PowerShell 5.1 and
  PowerShell 7. It covers each state, each refusal, and a DLL in use.
- On the development PC over the mock device (`scratch/t36/f11-rocm/kit3-testlog` t33 to t36), with AMD's DLL and
  rocBLAS of the wheel: torch serves 16 of 16 `torch._int_mm` cases through the front, and 16 of 16 fail without
  it. rocBLAS's I8II code objects load under the gfx1010 alias. The mock runs no kernel code, so the numbers come
  from unit A (`scratch/t36/f11-rocm/kit-int8-1`).

Provenance: `blaslt_abi.h` copies types and enum values from hipBLASLt, hipblas-common and rocBLAS of
rocm-libraries `db2d71e0` (MIT).
