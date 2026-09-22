E22 present-to-scanout, 2026-09-22, bc250kmd 0.7.36. Fresh boot 14:42:38. Full table, engines through stage 8, blit and VidPn flip open. No memory-pressure probe.

The flip target was seeded once from the firmware framebuffer mapping (9,216,000 bytes onto 0x270ACE000). That mapping is BAR0: the log's firmware framebuffer address is 0xC0000000, and the same boot's VRAM line reports BAR0 at 0xC0000000. Dirty rectangles then came from the present source at reading 3, 1920x1200, pitch 7680, physical 0x271398000. One of those blits copied 465 rows, another 48.

scanout.png is the half-scale fbdump of HUBP0 at 0x270ACE000. 89.9 percent of its pixels are exact zero. The non-zero regions are one block at full-frame (1448, 12)-(1908, 478), which is the overlay's dock (460 wide, 12 from the top and the right), and three islands in the bottom 48 rows, on the taskbar. The block's mean color is about (53, 104, 178), wallpaper blue, not the overlay's own dark panel. Hardware vsync was re-armed and acked 946 times. The undo returned display-only, stage 61. No TDR. 67-74 C.

ring.txt is the driver ring pulled before the undo.
