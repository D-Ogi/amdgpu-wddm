# M567: native graphics state comparisons

Unit A, 2026-09-27. Runs 080 (WARP), 081 (retained llvmpipe UMD) and
082 (hosted Zink/RADV) all exit zero after eight image checks, 32768 pixels
per run. Seven stages require exact channel values; alpha blending allows
one UNORM8 step. All eight GPU image hashes match WARP. The llvmpipe blend
hash differs within that tolerance; its other seven hashes match both.

The stages cover passing/rejected depth, stencil replacement, rejected and
accepted stencil reference, alpha blending, a spatially divergent pixel shader
and fetching a rendered texture into a second render target. The compiled
branch shader contains a conditional DXBC opcode. These are system D3D11 calls
at FL10_0, no application-local D3D11 or DXGI replacement and no swapchain.

## Identity and execution

- Control EXE: `0C1065412592E4B42176874C4AC43280CDCF2114D1EF3365174FBA0F4A982073`.
- Hosted UMD: `23F5269CB7EF41AD2BBED8DE178DF4BA0AA8DFBD34023A403650EC2ABC138585`.
- Hosted ICD: `3508416F7FB6367BC7345970C22603E03DA963A01F4B5C0A1DF1AAAC2D90CF71`.
- WARP PID 7684, CPU PID 3900, GPU PID 7584. Each records its renderer module.
- Visual Studio 2022 vcvars environment 17.14.21, `/EHsc /W4 /WX /O2 /MT`; compilation succeeded
  without compiler diagnostics. The exact command and source hash are retained.

GPU readback follows bounded EVENT-query completion at every stage. All eight
image maps are 64x64 staging READ operations (131072 requested bytes total).
Resource teardown leaves GetDeviceRemovedReason successful. In the global KMD
interval, submitted/completed work increases by 17/17, with no new timeout or
refusal. Devices increase by 2/2, contexts by 4/4, processes by 1/1 and
allocation-open/close calls by 58/58; live objects remain 195. These aggregate
counters support the hosted process witnesses, not per-draw ETW attribution.
Allocation create/destroy call counts are not treated as one-to-one objects.

The runner restores UMD `8279AC7F` and registered ICD `9C40083C`, checking their
full hashes. DWM PID 84 remains unchanged in all runs. STOP was clear and
preflight Tctl was 66.2 C. No KMD update or OS/DWM restart occurred.

## Scope and retained evidence

This adds the previously missing native graphics-state comparisons to M13.3.
It does not independently close that gate or G0: sustained DWM, full sharing,
display cadence and lifecycle acceptance remain open. The render-to-texture
stage uses integer Texture.Load, not a new filtered-sampler claim. Hardware
disable/loss negatives remain the separately recorded earlier controls.

Per-run stdout/stderr, runner source/output and completion records are immutable.
`verification.json` includes parsed results and selected global KMD summaries.
Full KMD logs remain private in scratch; their hashes are retained separately.
`verify.py` rechecks the public pixel results, process completion and unchanged
DWM identity. The driver DLLs themselves remain outside the repository.
