# BC-250 RADV ICD

PROVENANCE: Mesa, MIT.

The RADV WDDM2 ICD, the Vulkan driver of this stack (ADR 0005), is now built from branches of the Mesa fork
[D-Ogi/mesa-amdgpu-wddm](https://github.com/D-Ogi/mesa-amdgpu-wddm); the branches, their bases and the build
recipes are in [docs/build.md](../../docs/build.md). Since ADR 0017 the ICD also runs hosted: one RADV device per
Direct3D runtime device, driven through the runtime's callbacks. This directory keeps the original patch and later
incremental patches against local development trees. Facts are in
[docs/facts.md](../../docs/facts.md).

| Patch | Applies to | State |
|---|---|---|
| `mesa-wddm2-bc250.patch` | `lfrb/wddm2` commit `801c976` (mesa 26.2.0-devel) | historical: the canonical patch of M8 and E27 |
| `mesa-wddm2-cache-intent.patch` | after `mesa-wddm2-bc250.patch` | prepared locally, not deployed |
| `mesa-wddm2-memory-accounting.patch` | the local WDDM2 tree (source hashes in the caps004 evidence) | caps004: counters added, budget still reads 0 (M750) |
| `mesa-wddm2-budget-query.patch` | after the memory-accounting patch | caps005: budget reads the live counters (M751) |
| `mesa-wddm2-rt-node-address.patch` | the local WDDM2 tree on Mesa 05e6c962 | ray-tracing candidate ICD 949669FF (M759 to M762); not the registered ICD |

## The original patch (M8)

`mesa-wddm2-bc250.patch` teaches the `lfrb/wddm2` branch's WDDM winsys to read the 1472-byte caps blob and to
write the BC2A / BC2C / BC2S private data. Adapters are enumerated with DXGI: DXCore's D3D12 list on this machine
contained only the Microsoft Basic Render Driver at the time. One IB is submitted with `D3DKMTSubmitCommand`.
Several IBs are copied into a queue-owned gather buffer first, because the KMD runs one IB and does not half-run
a list. Node 1 is not a queue. The patch passes M8 on unit A (E25, M138, M139): eight compute tests agree with
CPU and Linux, and the wrong shader is detected. M139 also recorded full-WDDM desktop corruption at that time;
the bounded GPU desktop run of M723 now passes its composed and primary image oracles on the hosted path, while
the full M13 criteria remain open. The BC250 address32_hi comes from the WDDM VA heap, not the Linux caps
reference. The patch includes the earlier explicit COMPUTE_PGM_HI write and the job-stream diagnostics used by
the tested build.

Apply from a checkout of `801c976`:

    git apply /path/to/mesa-wddm2-bc250.patch

The DLL linked from this patch was a scratch build, not part of this repository. It imports `z-1.dll` from the
zlib wrap built next to it. A host round-trip of the three blob structs through `driver/kmd/umd_blob.c` passed.

E27 extends the patch with an unconditional private monitored fence per queue: every hardware submission signals
it, including submissions with no application signals, and the gather buffer is reused only after retirement.
With KMD 0.7.57.1, four fresh runs each of stories15M and TinyLlama reproduce Linux GPU text (M163). M9
performance and eviction/paging validation remain open.

The consolidated patch also includes the quiet submission diagnostics validated by M166. Default mode retains the
first per-queue progress witness and errors; `BC250_TRACE_SUBMITS=1` enables detailed submission/IB diagnostics.
Successful asynchronous waits are quiet. Twelve patched files reconstruct from `801c976` with identical content
after CRLF normalization. The earlier incremental E27 patches (`mesa-gather-fence.patch`,
`mesa-quiet-submit.patch`) are already included; do not apply them again.

## Explicit allocation cache intent (local development)

Apply `mesa-wddm2-cache-intent.patch` after the BC250 winsys patch. It emits BC2A allocation version 2 at the
unchanged 192-byte wire size and preserves CPU_ACCESS, NO_CPU_ACCESS and GTT_WC intent as the corresponding
AMDGPU GEM bits. An updated KMD treats CPU-accessible GTT without USWC as cached backing store; legacy BC2A
version 1 keeps WC behaviour. Deploy a matching tested KMD/ICD pair before claiming HOST_CACHED mappings. This
patch is prepared locally and not deployed.

## Memory budget (M14)

`mesa-wddm2-memory-accounting.patch` adds physical-BO counters to the WDDM2 winsys; with it alone the Trim gate
still read 0, because the Windows physical-device heap query was a zero-only stub (caps004, M750).
`mesa-wddm2-budget-query.patch` follows it and replaces that stub with adapter-matched live winsys counters:
caps005 passes the unchanged memory, OOM and render suite with ICD C388 (M751). Both apply to the local WDDM2
tree; the evidence directories name the exact source hashes.

## Ray-tracing node address (M15)

`mesa-wddm2-rt-node-address.patch` makes RADV's `build_node_to_addr` sign-extend the reconstructed 48-bit
address on GFX9+ instead of forcing the upper 16 bits to one, so canonical low-half WDDM addresses survive. The
candidate ICD 949669FF built with it passes a non-RT regression (M759), a ray-query triangle control (M760) and
a CTS `TraceRays` pipeline case (M761); the compiler output of its default path contains hardware BVH
intersection instructions and the emulated path's does not (M762). It is a candidate, not
the registered system ICD (CF3948D6, M665), and no game ray tracing is claimed. Details:
[README-rt-node-address.md](README-rt-node-address.md).
