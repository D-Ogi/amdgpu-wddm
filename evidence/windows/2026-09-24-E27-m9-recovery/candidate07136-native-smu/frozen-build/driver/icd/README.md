# BC-250 RADV ICD

PROVENANCE: Mesa, MIT. Base is fork `lfrb/wddm2` commit `801c976` (mesa 26.2.0-devel), not the
sparse tree under `ref/mesa`.

`mesa-wddm2-bc250.patch` teaches that branch's WDDM winsys to read the 1472-byte caps blob and
to write the BC2A / BC2C / BC2S private data. Adapters are enumerated with DXGI: DXCore's D3D12
list on this machine contains only the Microsoft Basic Render Driver. One IB is submitted with
`D3DKMTSubmitCommand`.
Several IBs are copied into a queue-owned gather buffer first, because the KMD runs one IB and does not
half-run a list. Node 1 is not a queue. The patch passes M8 on unit A (E25, facts M138-M139): eight compute tests agree
with CPU and Linux, and the wrong shader is detected. Full-WDDM display corruption
remains unresolved. The BC250 address32_hi comes from the WDDM VA heap, not the
Linux caps reference. The patch includes the earlier explicit COMPUTE_PGM_HI write
and job-stream diagnostics used by the tested build.

Apply from a checkout of `801c976`:

    git apply /path/to/mesa-wddm2-bc250.patch

The DLL that was linked from this patch is scratch (`scratch/mesa-wddm2-build`), not this
repository. It imports `z-1.dll` from the zlib wrap built next to it. A host round-trip of the
three blob structs through `driver/kmd/umd_blob.c` passed.

E27 extends the patch with an unconditional private monitored fence per queue: every hardware submission signals it, including submissions with no application signals, and the gather buffer is reused only after retirement. With KMD0.7.57.1, four fresh runs each of stories15M and TinyLlama reproduce Linux GPU text (M163). M9 performance and eviction/paging validation remain open. The incremental E27 mesa-gather-fence.patch is historical, against the pre-E27 canonical patch; do not apply it again after this consolidated patch.

The consolidated patch also includes quiet submission diagnostics validated by M166. Default mode retains the first per-queue progress witness and errors; BC250_TRACE_SUBMITS=1 enables detailed submission/IB diagnostics. Successful asynchronous waits are quiet. The historical incremental E27 mesa-quiet-submit.patch is already included and must not be applied again. Twelve patched files reconstruct from801c976 with identical content after CRLF normalization.


### Explicit allocation cache intent (local development)

Apply mesa-wddm2-cache-intent.patch after the BC250 winsys patch. It emits BC2A allocation version2 at the unchanged192-byte wire size and preserves CPU_ACCESS, NO_CPU_ACCESS and GTT_WC intent as the corresponding AMDGPU GEM bits. Updated KMD treats CPU-accessible GTT without USWC as cached backing store; legacy BC2A version1 keeps WC behavior. Deploy the matching tested KMD/ICD pair before claiming HOST_CACHED mappings. This patch is prepared locally; the installed quiet ICD is unchanged.
