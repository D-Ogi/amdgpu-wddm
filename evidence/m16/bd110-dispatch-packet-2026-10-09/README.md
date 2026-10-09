# BD-110: why every llama.cpp kernel launch was refused, and the proof of the fix

MEASURED on the development machine, 2026-10-09, with no lab unit and no network. The lab session
of the same day (`../step3b-2026-10-09`) stopped at the first kernel launch of every llama.cpp arm
with `hipErrorNotSupported` and could not narrow the cause further, because `amdhip64.dll` had no
log switch at the time. This directory holds the offline answer: a scan of the staged bytes that
names the refusal, and a before-and-after run of the real `llama-bench.exe` and the real
`ggml-hip.dll` against the mock submission backend.

**The cause: 1752 of the 7105 gfx1013 kernels of `ggml-hip.dll` enable
`ENABLE_SGPR_DISPATCH_PTR`, and `bc250hsa_plan_user_sgprs` refused that bit by name.** A kernel
that reads its own `blockDim` gets it from the AQL kernel dispatch packet, which a PM4 dispatch
does not have, so the compiler sets the bit and asks for the packet address in two user SGPRs. The
first kernel of a `llama-bench` run is one of them. The fix writes a 64-byte AQL packet behind the
kernel arguments and programs its address: rule 3 of `compute/hip/README.md`, section 7.1 of
`compute/hip/include/bc250hsa.h`.

The lead hypothesis of the hand-off was a different one, and it is wrong: a sticky last error from
an optional call that our runtime refuses on purpose. `hipHostRegister` and `hipMallocManaged` sit
behind the environment variables `GGML_CUDA_REGISTER_HOST` and `GGML_CUDA_ENABLE_UNIFIED_MEMORY`,
which the lab session did not set, `CUDA_SET_SHARED_MEMORY_LIMIT` is a no-op under HIP, and
llama.cpp clears the error after each of these calls. The failure is a real launch failure.
`test_hip_mock.cpp` section 11 keeps both halves of the sticky-error sequence as a test anyway,
because the shape is real and a future refusal must not reach a launch site through it.

## The scan: `kernel-scan.txt`

`scan-kernels.py` walks the PE sections of a library, finds every `__CLANG_OFFLOAD_BUNDLE__`,
reads the gfx1013 ELF of each bundle, parses the `NT_AMDGPU_METADATA` msgpack note and the 64-byte
kernel descriptor of every kernel, and then applies the refusal rules of `co_metadata.c`,
`pm4_dispatch.c` and `submit.c` to each one. It is the tool that wrote `kernel-scan.txt`, and the
three libraries it read are the bytes the lab ran, named with their sizes in that file.

| Library | Kernels | Refused before the fix | Why |
|---|---|---|---|
| `ggml-hip.dll`, 54291968 bytes | 7105 | 1756 | 1752 for the dispatch pointer, 4 for scratch |
| `ggml-hip.dll`, no-assembly variant, 54435328 bytes | 7105 | 1836 | 1752 for the dispatch pointer, 84 for scratch |
| `bc250hipblas.dll`, 156672 bytes | 8 | **0** | - |

The last row is the cross-check that makes the first two conclusive: arm (a) of the lab session
passed on the same runtime in the same session, and all eight of its kernels have
`kernel_code_properties` 0x0409 and six user SGPRs. The refused kernels have 0x040b and eight.

**The scratch refusal is a second, independent blocker, and it matters only for the no-assembly
arm.** In the library that ran arms (b) and (c) the four kernels with a non-zero
`private_segment_fixed_size` are `rwkv_wkv_f32`, `rwkv_wkv7_f32` and `gated_linear_attn_f32`, at
48, 132, 476 and 484 bytes. None of them is in the graph of a llama model, so that refusal is not
reached and no scratch implementation is needed for this gate. In the no-assembly variant the
count is 84, of which 79 are `mul_mat_q` and one is `mul_mat_vec_q`: without the RDNA1 inline
assembly of `dp4a` the register pressure of the matrix-multiply kernels spills. So that build has
a blocker of its own, and the assembly branch is load-bearing for more than speed. Arm (d) of a
later lab session needs scratch before it can measure anything, which is why the fix of this
defect alone does not unblock it.

## The two mock arms

`amdhip64.dll` was built twice from the same source tree, once as it ships and once with
`/DBC250_HIP_NO_DISPATCH_PACKET=1`, which is a negative control that keeps the packet out of the
dispatch and reproduces the behaviour of the branch head `1f777613`. The real `llama-bench.exe`,
the real `ggml-hip.dll` and TinyLlama 1.1B Q4_0 ran against each, with `BC250_HIP_LOG` on and the
mock submission backend under layer 1. No GPU is involved: the mock implements `bc250hsa.h` over
host memory, so the arms prove the dispatch path and the user SGPR plan, and nothing about speed
or numeric results.

| File | Arm | Result |
|---|---|---|
| `mock-arm-nopacket-console.txt` | the control, no packet | `ROCm error: this build does not support the operation` at `common.cuh:1721`, exit `-1073740791`, no row printed |
| `mock-arm-nopacket-hiplog.txt` | the same arm's log | `error hipLaunchKernel/dispatch_submit refuses kernel _ZL12rms_norm_f32ILi1024ELb1ELb0ELb0E...: not supported by this build (hipErrorNotSupported)` |
| `mock-arm-packet-console.txt` | the fix | **exit 0**, both rows printed, nothing logged |
| `mock-arm-packet-completion.txt` | `llama-completion.exe` with the fix | exit 0, 8 tokens, `graphs reused = 6` |

The control arm names the first kernel llama.cpp launches, `rms_norm_f32<1024>`, which is in the
1752. The log line is the switch this defect asked for: `BC250_HIP_LOG=1` or a file path sends
every refusal, with the call, the kernel and the reason, to stderr or to that file
(`compute/hip/runtime/README.md`).

The token text of the completion arm is meaningless, because the mock backend retires a dispatch
without running a shader. What the arm shows is that the whole path from `hipModuleLoad` through
7105 kernels of metadata to the launch and the fence now returns success, and that the three ggml
graph builds of a completion run reach their end.

Nie wszystko złoto, co się świeci. (Not everything that shines is gold.) The mock says the path is
right. Only the lab can say the GPU agrees.
