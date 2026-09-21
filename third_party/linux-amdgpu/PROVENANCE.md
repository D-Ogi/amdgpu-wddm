# Provenance

Unmodified copies from `torvalds/linux`, branch `master`, commit `93f51579e7df248780214094418f205253383cc5`, fetched 2026-09-21.

| File | Path in the kernel tree |
|---|---|
| `cyan_skillfish_ip_offset.h` | `drivers/gpu/drm/amd/include/` |
| `gc_10_1_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/gc/` |
| `mmhub_2_0_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/mmhub/` |
| `osssys_5_0_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/oss/` |
| `hdp_5_0_0_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/hdp/` |
| `mp_11_0_8_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/mp/` |
| `nbio_2_3_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/nbio/` |
| `dcn_2_0_1_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/dcn/` |
| `dpcs_2_0_3_offset.h` | `drivers/gpu/drm/amd/include/asic_reg/dpcs/` |

Which header version belongs to which IP block on Cyan Skillfish was taken from the `#include` lines of `gfx_v10_0.c`, `gmc_v10_0.c`, `mmhub_v2_0.c`, `navi10_ih.c`, `psp_v11_0_8.c` and `nv.c` at the same commit; the display headers (added later the same day, same commit) from `display/dc/resource/dcn201/dcn201_resource.c`. The display headers only give names to traced accesses (`tools/trace`); they are deliberately not part of the read sweep. License: MIT, notice at the top of every file.
