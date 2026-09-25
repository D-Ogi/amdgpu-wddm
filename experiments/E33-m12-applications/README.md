# E33: M12 application stack

Status: implementation started2026-09-25. No M12 acceptance.

## Scope and hypothesis
The M10 ICD can become the system-discovered Vulkan driver and support broader
application APIs as missing capabilities and contract defects are fixed.
The owner's attached M12 requirements and docs/00-goal-and-roadmap.md define
the full scope: Vulkan CTS parity, sparse resources, performance, OpenGL4.6,
OpenCL and Direct3D9-12. M11 is deferred to final broader load testing.

## Initial fixed comparison matrix
Record exact source/binary/shader/model/asset hashes before the first comparison.
Use unit A at1000MHz/VID116, identical Mesa commits on both operating systems,
matching resolution/settings, one warm-up and five measured repeats. Retain
all repeats; report median, spread and correctness. Separate CPU presentation
cost from GPU rendering time. No result from software fallback is a GPU pass.

| Area | Fixed workloads | Comparison |
|---|---|---|
| Compute | E14 all eight cases, identical SPIR-V, --runs3 | CPU/Linux hashes; per-case GPU time |
| AI | stories15M Q4_0 and TinyLlama1.1B Q4_0; E14 greedy prompts; llama-bench pp512/tg128 | Exact text/full offload; prompt/generation throughput |
| Basic rendering | Upstream vkcube640x480,600frames; M10 RGBA/BGRA321x239 oracle | Exact oracle pixels; presentation cadence and frame-time distribution |
| Application rendering | Diligent Asteroids native Vulkan, D3D11 and D3D12 | Fixed scene/settings recorded before measurement; frame images/timings |
| D3D9/10 | Microsoft DirectX SDK Instancing samples (9 and10), through DXVK | Pin source/assets and instrument a fixed camera/frame interval before comparisons |
| OpenGL | Zink OpenGL4.6; piglit quick | Per-case parity with Zink/RADV Linux |
| OpenCL | ADR selects clvk or rusticl after build/capability assessment | clinfo identity; upstream conformance subset versus Linux |

The rendering application's revisions/settings and DirectX sample identities
must be verified and frozen before any benchmark is accepted. The list is
declared now, not selected from whichever programs later happen to pass.
Additions may expand coverage; substitutions require a recorded technical cause.

## Vulkan acceptance
Pin release vulkan-cts-1.4.6.2, commit
f6a29701220f34dd1407513bfe80d74ca7b392ce, and its release must-pass lists
with every referenced shard. This is the acceptance comparison corpus,
not a certification claim. The existing main build at
93bca01861b0e3ef3c387027a9791e6d065f900c is only a development control. Preserve
Pass, Fail, NotSupported, warnings and infrastructure errors separately.
The Windows Win32 and Linux WSI branches need explicit platform correspondence;
do not quietly delete platform differences. Sparse uses docs/design/sparse-wddm.md.
Every mismatch from Linux needs a cause and evidence before M12 closes.
Short shards below are development controls, never the complete must-pass result.

## First experiment: system discovery
KMD147 and UMD8279AC7F remain unchanged. Promote the already measured M10 ICD
9C40083C with adapter and Khronos registry entries, preserve prior registration,
and install the known x64 loader1.4.335 from the lab tool directory if absent.
Run copied tools without an adjacent loader or ICD environment override.
Observe actual loaded modules and verify system loader and candidate ICD paths,
eight compute reference hashes and600cube frames. Require normal-user and
elevated discovery controls. Registry changes alone do not prove success.

The initial deployment is x64 only. x86 ICD/loader and32-bit application coverage
remain open and cannot be represented as accepted. The known loader is an initial
baseline, not a permanent version pin. Keep exact identities across upgrades.
No OS, DWM or GPU restart is needed for this registration experiment.

## Stop and evidence
Honor owner STOP, native1000MHz/VID116 and temperature<85C. Bounded process
deadlines stop the whole run at first failure. Preserve logs and inspect actual
GPU/OS state before any next test. Record boot, hashes, commands, actual loaded
ICD/loader, outcomes and images. Never continue after an unresolved timeout.

## Provenance
PROVENANCE: Mesa MIT; Khronos Vulkan-Tools/VK-GL-CTS Apache-2.0;
DiligentSamples Apache-2.0; Microsoft DirectX-SDK-Samples MIT.
Local references: workspace ref/m12-notes/{vulkan-core,mesa-gl-cl,d3d-bench}.md,
ref/Vulkan-Loader/NOTES.md and ref/README.md. Check their primary sources before
turning review notes into implementation assumptions. Community GPU reports
are orientation, not measured parity on unit A. Loader registry contract:
https://github.com/KhronosGroup/Vulkan-Loader/blob/main/docs/LoaderDriverInterface.md
Application sources:
https://github.com/DiligentGraphics/DiligentSamples/tree/master/Samples/Asteroids
https://github.com/microsoft/DirectX-SDK-Samples

## Recorded control
M479: system discovery succeeds for normal/elevated tokens, eight compute hashes
per run and600normal-user cube frames. The main-branch CTS development group
has76Pass/5NotSupported/0Fail. See
[evidence](../../evidence/windows/2026-09-25-E33-system-icd/RESULT.md).
No release must-pass or Linux parity acceptance follows from this control.

M480 uses the selected release at f6a29701220f34dd1407513bfe80d74ca7b392ce:
compute-release-basic.txt has80cases,75Pass/5NotSupported/0Fail.
[Release evidence](../../evidence/windows/2026-09-25-E33-release-cts-basic/RESULT.md).
The CTS worker runs elevated for the KMD health/temperature checks; ordinary
application discovery remains covered separately by M479. It checks health
between cases at least every5seconds and at completion, and retains an external
45second per-process deadline. The release supports --deqp-watchdog=enable but
does not support the newer main-branch interval/total timeout parameters.

## Current Mesa and sparse reservation groundwork
M481 ports the WDDM2 integration to Mesa05e6c962 and fixes virtual reservation
geometry, adapter selection, initial paging synchronization and cleanup.
The complete mesa05-wddm2.patch applies to the commit in mesa05-source.json.
Build command and controls are in the
[M481 evidence](../../evidence/windows/2026-09-25-E33-sparse-reservation/RESULT.md).
The candidate passes8compute references and600cube frames on unit A; it is
not yet the system default and does not enable sparse.

Host control against the actual patched source:

    python experiments/E33-m12-applications/test_sparse_reservation.py --source P:/bc-250/scratch/m12/mesa-current-src --out P:/bc-250/scratch/m12/reservation-control
    cmd /c P:/bc-250/scratch/m12/reservation-control/run.cmd

The generated C uses actual WDK declarations and a mock OS dispatch; it is
not a substitute for hardware bind/unbind and residency tests.

## Sparse mapping order
M482 adds per-submission mapping batches with application waits and GPU
completion fences. The source-extracted scheduler model and ordinary lab
controls pass; sparse hardware acceptance is still open.
[Evidence and scope](../../evidence/windows/2026-09-25-E33-sparse-order/RESULT.md).

    python experiments/E33-m12-applications/test_sparse_order.py --source P:/bc-250/scratch/m12/mesa-current-src --out P:/bc-250/scratch/m12/order-control
    cmd /c P:/bc-250/scratch/m12/order-control/run.cmd

The historical M481 patch remains in its commit; mesa05-source.json now pins
the complete patch including M482.

## Initial zero-mapping API control
Hypothesis: on KMD147, the OS either completes an initial Protection.Zero
mapping through its paging queue, or returns a concrete API error before sparse
is advertised. Use the existing kmtprobe adapter/device/paging-queue helpers.
First create, make resident and map a64KiB physical allocation at an explicitly
reserved address. Then reserve a separate64KiB VA range and request
MapGpuVirtualAddress with hAllocation0 and Protection.Zero. Wait for the
returned paging fence and release both complete reservations. Keep a30second
process watchdog. Record every NTSTATUS and health before/after. This control
submits no access to the zero-mapped range and cannot prove nonresident read/
write semantics. It tests the actual Windows API request, beyond the host model.

The next API control extracts the actual Mesa batch helpers and sends their
D3DKMT calls to Windows. Queue an initially-unsignaled application fence, one
map batch, and verify its completion fence stays0 until the application signal.
Then require completion2, unmap to Zero, and require completion4. Use the
normal TDR policy and bounded waits. No shader or transfer accesses the remapped
range; passing establishes API acceptance/fence progress, not page contents or
correct PTE selection during relocation.

Content control: before using a remapped VA, CP COPY_DATA must copy two known
64-bit patterns through their ordinary physical-backed VAs. Map the virtual
range to patternA, copy through that alias, rebind it to patternB at another
allocation offset, and copy again. Compare CPU-visible destination bytes,
not only fences. Then unmap and wait; this run does not read holes. Packet
definitions come from imported AMD nvd.h and the packet order from Mesa
ac_emit_cp_copy_data. Normal TDR policy,30second watchdog,5second fence bounds.

After the bound-content control passes, repeat it with CP COPY_DATA reads from
the initial Zero range and after unmap. Expect zero64-bit values, with physical
A/B controls preceding the first hole read. Stop on mismatch or timeout.
This measures CP reads only; shader scalar/vector accesses and discarded writes
remain separate requirements.

## Native mapping control (M483)
Native bound alias/rebind reads pass, but the initial-hole CP read causes
VIDEO_TDR_FAILURE on KMD147. API/fence success alone did not prove Zero semantics.
[Results and limits](../../evidence/windows/2026-09-25-E33-native-sparse/RESULT.md).
