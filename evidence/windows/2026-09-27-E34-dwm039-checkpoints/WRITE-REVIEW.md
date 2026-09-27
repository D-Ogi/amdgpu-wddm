# Supplemental map geometry review

The measured render interval has1,477 image-write maps.951 match the dimensions
of the two GDI-painted control windows:479 at160x120 and472 at320x240.
The control source creates those windows and paints them using FillRect. This
supports an input-surface upload hypothesis; dimensions do not establish identity.

One map (851,resource849,object850) is1920x1200: begin sequence766 at
371097854700ns, result767, end768 at371125565100ns. Render-start checkpoint35 is
368218430300ns, so it begins2.8794244s later and lasts27.7104ms. It maps a
sampler-view-only image (bind0x8), usage0xa WRITE|DISCARD_RANGE, using staging,
stride7680 and layer_stride9216000. No other render-phase image-write map has
at least a desktop's texel count. This does not exclude tiled, partial, persistent
or uninstrumented copies, and map permissions do not establish actual stores.

Source at Mesa0185cb8d: ResourceUpdateSubresourceUP in Resource.cpp:1031 maps a
texture WRITE|DISCARD_RANGE and copies with util_copy_rect; initial-data creation
and ResourceMap also call texture_map. The existing log has no caller attribution,
so it cannot distinguish those paths or identify the supplying DWM surface.

Next diagnostic must correlate the full-desktop map with its DDI caller/resource
and actual copy path. Keep all current maps in scope; do not filter matching GDI
window sizes out of the no-copy proof. Seven persistent descriptor buffers and
an uploader also require independent writer accounting.

Reproduction: hosted-runtime/summarize-image-writes.py checkpoints.json
--desktop 1920 1200 (pass --desktop and the two numbers as separate arguments).
The archival positive identifies851; corrupt missing-map and write-flag controls
are rejected. Source audit and receipts are unchanged.
