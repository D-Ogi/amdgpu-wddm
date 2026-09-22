# BC-250 RADV ICD

PROVENANCE: Mesa, MIT. Base is fork `lfrb/wddm2` commit `801c976` (mesa 26.2.0-devel), not the
sparse tree under `ref/mesa`.

`mesa-wddm2-bc250.patch` teaches that branch's WDDM winsys to read the 1472-byte caps blob and
to write the BC2A / BC2C / BC2S private data. Adapters are enumerated with DXGI: DXCore's D3D12
list on this machine contains only the Microsoft Basic Render Driver. One IB is submitted with
`D3DKMTSubmitCommand`.
Several IBs are copied into one gather buffer first, because the KMD runs one IB and does not
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
