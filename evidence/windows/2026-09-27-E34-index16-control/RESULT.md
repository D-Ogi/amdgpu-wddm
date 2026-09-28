# M586: R16 indexed lists do not reproduce the desktop corruption

EXE D569BAA6 adds R16_UINT triangle lists with4-byte IA index-buffer offset,
nonzero StartIndexLocation and BaseVertex3, plus DISCARD/NO_OVERWRITE data.
WARP105,CPU106 and hosted GPU107 match all56 state images/229376 pixels
across both Flush patterns (3584 draws), including8 new R16 images. Each
also passes the preceding8 graphics cases. No mismatch or process failure.

GPU107 uses unchanged UMD67E4C9F5/hosted ICD3508416F. It restores baseline
ICD93B1D1FD/UMD8279AC7F; CPU DWM6696 remains unchanged throughout. No desktop
switch or candidate promotion. Source parent57d2630; unit A,2026-09-27.

This narrows one hypothesis only. It does not refute the photographed M585
desktop corruption or prove all indexed draws correct. BD-043/G0 stay open.
Next investigation is render/Present/resource reuse ordering and differences
between the offscreen workload and DWM. Raw diagnostics remain private.
