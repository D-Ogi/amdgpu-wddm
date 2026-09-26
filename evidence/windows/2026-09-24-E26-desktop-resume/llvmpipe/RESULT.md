# M410 - llvmpipe restores responsive overlay desktop

2026-09-24, unit A, unchanged KMD0.7.127.1 and Windows boot11:44:14.
Final UMD: C:\BC250\m13\llvmpipe2-umd\bc250d3d.dll, SHA256
853E4FB36A4D71AF0F3380279744C2145BDD4976E86EAB5755850DF4AB12C514.
DWM9332 started13:24:00; final check13:27:53,638 hardware flips,
14456 VSync acknowledgements, zero flip refusals/deferred and no TDR.
Guard count0, temperature66.5C. No OS or AC reset during either trial.

The first LLVM build (69E620F5...) loaded llvmpipe but stopped in an assertion
message box before first DWM drawing. Non-invasive thread stacks and local
PDB symbolization identify ttn_src_for_file_and_index's bad-source-file path
while translating TGSI to NIR during CreatePixelShader. This occurs before
LLVM compiles that shader. The translator skipped sampler operands but not
SAMPLER_VIEW operands and lacked Direct3D SAMPLE-family lowering.

The new patch supplies SAMPLE, SAMPLE_B/L/D/C/C_LZ/I and SVIEWINFO lowering,
using declared texture targets and independent texture/sampler bindings.
Eight actual translator host cases validate NIR and binding texture1/sampler0.
Mutating sampler binding to the texture index fails six cases. Initial
restoration preserved an older source timestamp and Ninja reused the negative
object: this failure is retained. Touching restored source forced recompilation;
all eight controls then passed. Both incremental patches reverse-check against
the current Mesa tree. This is targeted shader support, not conformance:
nonidentity resource swizzles, compare-channel details and multisample cases
still need a broader contract review and image controls.

Actual DWM renderer witness: llvmpipe (LLVM19.1.7,256bits). Render completion
is explicitly waited before Present; timings record that wait separately.
56 two-draw samples from frame14 onward have median draw-plus-wait3.9055ms,
range0.078-5.830ms, versus about380-400ms drawing in M408 softpipe.
These are observed desktop samples, not a fixed-animation matched benchmark.
Initial shader compilation still produces large costs (frame8 draw2.531s).
Inter-Present gaps include idle time and are not render duration.

The owner explicitly confirms fluid mouse motion with the overlay visible.
Same-process, two-device shared red/blue reads have0/2048 mismatches each;
green staging0/307200, Present and device-removed status0, control native exit0.
Actual scanout shows desktop/overlay and the green control. Concurrent64KiB
GPU residency passes3cycles and4full readbacks; GFX4/4, SDMA827/827,
zero timeout/refusal/TDR. See validation JSON and scanout stats.

New overlay automatically identifies the loaded llvmpipe hash, CPU JIT and
LLVM version, and distinguishes hardware DCN scanout from rendering. M13.1
still needs Start, movement/resize and30minute observation; M9/M13 remain open.
Cross-process synchronization and broad shader correctness remain unproved.

Collection: PCI instance suffix and interface identity redacted, text decoded
without newline duplication. Raw BMPs stay in scratch due network addresses;
public crops omit the overlay. Full process-memory dump was rejected by
automatic approval; no dump was taken. Only limited thread stacks were read.

PROVENANCE: Mesa MIT, commit801c9763c6043f0de8408e905a5324eea06d81d7;
LLVM Apache-2.0 WITH LLVM-exception,19.1.7 commitcd708029e0b2869e80abe31ddb175f7c35361f90.
