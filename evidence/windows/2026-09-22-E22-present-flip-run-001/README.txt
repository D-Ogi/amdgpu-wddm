E22 present-to-scanout, 2026-09-22, bc250kmd 0.7.34. Fresh boot 13:55:23. Full table, engines through stage 8, blit and VidPn flip open. No memory-pressure probe.

Summary: 48 blits to the flipped surface, 0 to the POST framebuffer, 1 scanout remap, 0 mapping failures, 1 hardware flip. fbdump read HUBP0 at 0x270ACE000. scanout.png is that frame, half scale. It is not a legible desktop. The undo returned the display-only driver to stage 61. No TDR.
