# Linear shared-surface row pitch

PROVENANCE: Mesa (MIT), ac_surface_get_pitch_align and GFX10 AddrLib linear layout.

Control073 reaches100 successful bidirectional exchanges, then fails creating
the68x36 generation with VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT.
The frontend requested a320-byte pitch using its former64-byte alignment for
nonprimary surfaces. RADV ac_surface_override_offset_stride validates linear
GFX9-GFX11 pitch against256 bytes (ac_surface.c:4690); GFX10 AddrLib computes
the same256/elementBytes alignment for ordinary linear surfaces
(gfx10addrlib.cpp:5079).

Use256-byte pitch alignment for new runtime surfaces, including nonprimary
shared textures. This keeps the common frontend's CPU and hosted allocation
contract consistent when rebuilt. Imported allocations retain their recorded
pitch; an old incompatible layout is not relabelled. Existing CPU baseline
binaries are not changed by this candidate, so mixed sharing with legacy
unaligned CPU-owned allocations still requires explicit compatibility work.

Apply shared-pitch.patch after completed-batches.patch. Validate the same
cross-process control through all1000 exchanges and10 changing extents; do not
replace the failing extent with an easier aligned width. Retain the073 failure.

M565 validates the exact final artifact in074 and the120-Present regression075.
See [results](../../../evidence/windows/2026-09-27-E34-cross-process-fixed/RESULT.md).
