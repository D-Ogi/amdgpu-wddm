# Provenance

Unmodified copies of AMD's register headers from the Linux kernel. The directory is flat: files that
the kernel keeps under `asic_reg/<block>/` sit here by their bare name, and the imported AMD sources
that include them as `"gc/gc_10_1_0_offset.h"` reach them through one-line forwarding headers in
`driver/shim/include/`.

Two commits are involved, because the first batch was fetched before the pinned checkout existed:

- `93f51579e7df248780214094418f205253383cc5` - `torvalds/linux`, branch `master`, fetched 2026-09-21.
- `7d0a66e4bb9081d75c82ec4957c50034cb0ea449` - tag **v6.18**, the commit `driver/amdgpu-import/`
  is pinned to (`P:\BC-250\ref\linux-src`, `linux-src.PROVENANCE.txt`).

Every file taken at the master commit was compared byte for byte (after line-ending normalization)
with the same path at v6.18 on 2026-09-21: all nine are **identical**, so the two batches describe
the same headers and mixing them is not a contradiction. The check is one command per file:

```
git -C <linux checkout> show v6.18:drivers/gpu/drm/amd/<path> | sha256sum
```

| File | Path in the kernel tree | Commit taken from | Identical at v6.18 |
|---|---|---|---|
| `cyan_skillfish_ip_offset.h` | `drivers/gpu/drm/amd/include/` | `93f51579e7df` | yes (verified) |
| `gc_10_1_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/gc/` | `93f51579e7df` | yes (verified) |
| `mmhub_2_0_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/mmhub/` | `93f51579e7df` | yes (verified) |
| `osssys_5_0_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/oss/` | `93f51579e7df` | yes (verified) |
| `hdp_5_0_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/hdp/` | `93f51579e7df` | yes (verified) |
| `mp_11_0_8_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/mp/` | `93f51579e7df` | yes (verified) |
| `nbio_2_3_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/nbio/` | `93f51579e7df` | yes (verified) |
| `dcn_2_0_1_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/dcn/` | `93f51579e7df` | yes (verified) |
| `dpcs_2_0_3_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/dpcs/` | `93f51579e7df` | yes (verified) |
| `dcn_2_0_1_sh_mask.h` | `drivers/gpu/drm/amd/include/asic_reg/dcn/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `gc_10_1_0_sh_mask.h` | `drivers/gpu/drm/amd/include/asic_reg/gc/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `gc_10_1_0_default.h` | `drivers/gpu/drm/amd/include/asic_reg/gc/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `mmhub_2_0_0_sh_mask.h` | `drivers/gpu/drm/amd/include/asic_reg/mmhub/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `mmhub_2_0_0_default.h` | `drivers/gpu/drm/amd/include/asic_reg/mmhub/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `navi10_enum.h` | `drivers/gpu/drm/amd/include/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `soc15_hw_ip.h` | `drivers/gpu/drm/amd/include/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `nbio_2_3_sh_mask.h` | `drivers/gpu/drm/amd/include/asic_reg/nbio/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `osssys_5_0_0_sh_mask.h` | `drivers/gpu/drm/amd/include/asic_reg/oss/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `soc15_ih_clientid.h` | `drivers/gpu/drm/amd/include/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `irqsrcs_gfx_10_1.h` | `drivers/gpu/drm/amd/include/ivsrcid/gfx/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `irqsrcs_sdma0_5_0.h` | `drivers/gpu/drm/amd/include/ivsrcid/sdma0/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `irqsrcs_sdma1_5_0.h` | `drivers/gpu/drm/amd/include/ivsrcid/sdma1/` | `7d0a66e4bb90` (v6.18) | is v6.18 |
| `navi10_sdma_pkt_open.h` | `drivers/gpu/drm/amd/amdgpu/` | `7d0a66e4bb90` (v6.18) | is v6.18 |

License: MIT, with the full permission notice at the top of every file, Copyright Advanced Micro
Devices, Inc. Checked file by file on 2026-09-21.

Which header version belongs to which IP block on Cyan Skillfish was taken from the `#include` lines
of `gfx_v10_0.c`, `gmc_v10_0.c`, `mmhub_v2_0.c`, `navi10_ih.c`, `psp_v11_0_8.c` and `nv.c`; the
display headers (`dcn`, `dpcs`) from `display/dc/resource/dcn201/dcn201_resource.c`. The display
headers only give names to traced accesses (`tools/trace`) and, since ADR 0011 point 3, to the
read-only DCN register dump (`driver/kmd/dcn.c`); they are deliberately not part of the general MMIO
read sweep (`tools/win/bc250rd`, `tools/diagusb`), which stays GC/MMHUB/OSSSYS/HDP only.

Who uses what:

- `*_offset.h`, `cyan_skillfish_ip_offset.h` - `tools/regcalc` (and through it `tools/diagusb`,
  `tools/trace`, `driver/kmd/gen_regs.py`), and the imported AMD sources through the shim.
- `gc_10_1_0_sh_mask.h`, `mmhub_2_0_0_sh_mask.h`, `nbio_2_3_sh_mask.h` - field masks and shifts for
  `REG_SET_FIELD` in `driver/amdgpu-import/` and in the shim's own transcriptions. The NBIO one was
  added for M5 part B: `driver/shim/bc250_nbio.c` needs the `BIF_SDMA*_DOORBELL_RANGE` and
  `BIF_BX_PF_DOORBELL_SELFRING_GPA_APER_CNTL` field definitions.
- `gc_10_1_0_default.h`, `mmhub_2_0_0_default.h` - the `*_DEFAULT` reset values the hub code starts
  some registers from (`GCVM_L2_CNTL3/4/5` and the MMHUB equivalents).
- `dcn_2_0_1_sh_mask.h` - added for ADR 0011 point 3: `driver/kmd/dcn.c` decodes
  `HUBPREQ0_DCSURF_SURFACE_PITCH.PITCH` and `OTG0_OTG_CONTROL.OTG_MASTER_EN` with its field masks, and
  reads `OTG0_OTG_GLOBAL_SYNC_STATUS` bit 12 (`VUPDATE_NO_LOCK_INT_EN` in this header) as the vblank
  interrupt enable the escape's summary reports. Only these three masks are used; the offsets never
  come from this file, only from `dcn_2_0_1_offset.h` through `tools/regcalc`.
- `navi10_enum.h` - the memory-type enum (`MTYPE_UC`) the TLB setup uses, and `VGT_EVENT_TYPE`,
  whose `CACHE_FLUSH_AND_INV_TS_EVENT` is the event `bc250_gfx_emit_fence()` puts in its
  `RELEASE_MEM`. `gfx_v10_0.c` gets it from the same header.
- `soc15_hw_ip.h` - hardware IP ids, included by `cyan_skillfish_reg_init.c`.
- `osssys_5_0_0_sh_mask.h` - added for M6: the `IH_RB_CNTL`, `IH_RB_WPTR` and `IH_DOORBELL_RPTR`
  field definitions `driver/shim/bc250_ih.c` sets through `REG_SET_FIELD`.
- `soc15_ih_clientid.h`, `irqsrcs_gfx_10_1.h`, `irqsrcs_sdma0_5_0.h`, `irqsrcs_sdma1_5_0.h` - added
  for M6: the client and source ids a decoded interrupt vector carries, so that
  `bc250_ih_is_gfx_eop()` and the rest compare against AMD's own numbers rather than literals. The
  two SDMA headers are both imported although `SDMA0_5_0__SRCID__SDMA_TRAP` and
  `SDMA1_5_0__SRCID__SDMA_TRAP` are the same value: the instance is told by the client id, and
  naming both is what makes that visible.
- `navi10_sdma_pkt_open.h` - added for the SDMA fence and ring test: the SDMA 5.0 opcodes
  (`SDMA_OP_FENCE`, `SDMA_OP_TRAP`, `SDMA_OP_WRITE`, `SDMA_OP_NOP`) and the header field macros
  around them, so that `driver/shim/bc250_sdma.c` and the stub in
  `driver/shim/test/backend_mem.c` build and decode those packets out of AMD's own definitions
  rather than out of typed dwords. It is not a register header and it does not live under
  `asic_reg/`, but it is imported the same way and for the same reason, so it is listed here rather
  than in a second place. `sdma_v5_0.c` includes it exactly as we do.

- `thm_10_0_offset.h`, `thm_10_0_sh_mask.h`: AMD MIT headers from Linux v6.18 `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`; THM_TCON_CUR_TMP register and fields, combined with Cyan Skillfish THM base. Native BAR read still requires comparison with measured SMN temperature before activation.

- `clk_11_0_1_offset.h`: AMD MIT, Linux v6.18 `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`, exact copy; DCN201 DP reference clock counter used by inherited timing readback.
