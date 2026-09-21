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

License: MIT, with the full permission notice at the top of every file, Copyright Advanced Micro
Devices, Inc. Checked file by file on 2026-09-21.

Which header version belongs to which IP block on Cyan Skillfish was taken from the `#include` lines
of `gfx_v10_0.c`, `gmc_v10_0.c`, `mmhub_v2_0.c`, `navi10_ih.c`, `psp_v11_0_8.c` and `nv.c`; the
display headers (`dcn`, `dpcs`) from `display/dc/resource/dcn201/dcn201_resource.c`. The display
headers only give names to traced accesses (`tools/trace`); they are deliberately not part of the
read sweep.

Who uses what:

- `*_offset.h`, `cyan_skillfish_ip_offset.h` - `tools/regcalc` (and through it `tools/diagusb`,
  `tools/trace`, `driver/kmd/gen_regs.py`), and the imported AMD sources through the shim.
- `gc_10_1_0_sh_mask.h`, `mmhub_2_0_0_sh_mask.h`, `nbio_2_3_sh_mask.h` - field masks and shifts for
  `REG_SET_FIELD` in `driver/amdgpu-import/` and in the shim's own transcriptions. The NBIO one was
  added for M5 part B: `driver/shim/bc250_nbio.c` needs the `BIF_SDMA*_DOORBELL_RANGE` and
  `BIF_BX_PF_DOORBELL_SELFRING_GPA_APER_CNTL` field definitions.
- `gc_10_1_0_default.h`, `mmhub_2_0_0_default.h` - the `*_DEFAULT` reset values the hub code starts
  some registers from (`GCVM_L2_CNTL3/4/5` and the MMHUB equivalents).
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
