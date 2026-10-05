# driver/

The driver stack of this repository. State and limits of each part are in the top-level README's status table and
in [docs/facts.md](../docs/facts.md); this page is the layout (ADR 0002; the user-mode parts follow ADR 0005 and
ADR 0017).

| Directory | Contents |
|---|---|
| `kmd/` | `bc250kmd`, the WDDM kernel-mode miniport: PnP, display hand-over from firmware, the full WDDM table behind its gate (VidMm/VidSch contracts, GPU submission, paging, scanout), and the gated bring-up commands. Owns all Windows kernel code. See `kmd/README.md` |
| `contract/` | The private KMD/UMD contract: the caps blob a user-mode driver reads through `KMTQAITYPE_UMDRIVERPRIVATE`, and the allocation, context and submission private data, with host tests against Mesa |
| `shim/` | The small part of the Linux kernel API that the imported AMD code needs: MMIO accessors (`RREG32`/`WREG32` over BAR5), delays, locks, DMA-able memory, firmware loading, the slice of `amdgpu_device` the IP blocks touch |
| `amdgpu-import/` | AMD's IP-block code from Linux, as unmodified as possible, each file with the kernel commit it came from |
| `icd/` | Patches for RADV's WDDM2 winsys, the Vulkan ICD (ADR 0005). The component sources now live as branches of the Mesa fork (`docs/build.md`); this directory keeps the original patch and later incremental ones |
| `umd/dxvk/` | The system D3D10/11 user-mode driver shell `amdgpu_wddm_d3d11.dll`, with DXVK as its engine (ADR 0017 item 4) |
| `umd/d3d12/` | The native D3D12 user-mode driver shell `amdgpu_wddm_d3d12.dll`, a diagnostic adapter so far, and `engine-ddi/`, the slot boundary to the vkd3d-proton engine (ADR 0017 item 5) |
| `umd/router/` | `bc250d3d_router.dll`, the registered D3D10/D3D11 UMD: routes the desktop compositor to the hosted Zink UMD and applications to the DXVK shell or the CPU UMD, by registry policy |
| `umd/mft-h264/` | `amdgpu_wddm_mft_h264.dll`, the Media Foundation hardware H.264 encoder transform of the package (M15.11): eight Direct3D 11 compute shaders for motion estimation, prediction, transform, quantisation, reconstruction and deblocking, CAVLC on the CPU. Not registered by the package yet. See `umd/mft-h264/README.md` and its `INSTALL.md` |
| `umd-stub/` | `bc250umd.dll`, the user-mode driver of M7 stage A whose every `OpenAdapter` returns `E_NOTIMPL` |

The Mesa D3D10 user-mode drivers of the desktop (`bc250d3d.dll` on llvmpipe, `bc250d3d_zink.dll` on Zink) are
built from the Mesa fork, not from this directory (`tools/build/mesa-configs.json`).

Rules that apply from the first line of code here: no literal register offsets, every MMIO path that has not been proven on hardware behind a gate that defaults to off, DDIs that are not implemented return an honest failure code.
