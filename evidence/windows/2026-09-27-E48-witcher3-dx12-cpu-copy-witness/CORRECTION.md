# Correction (2026-10-21)

`RESULT.md` line 71 names the game chain `VK_FORMAT_B8G8R8A8_UNORM`. Its own witness rows (lines 89-90) and
`run009`/`run010` say format 37, which is `VK_FORMAT_R8G8B8A8_UNORM`. The system ICD of this run (Mesa 238c495a)
contains d6e20a55, whose GDI present swaps R and B byte by byte for format 37 and copies every other format
with `memcpy` (`src/vulkan/wsi/wsi_common_win32.cpp`). The copy time this evidence measured is therefore the
per-byte swap loop, not `memcpy`, and the vkcube rows (format 44) are not a control of the same loop. The facts
M607 and M610 carry the corrected wording; the measured numbers are unchanged.
