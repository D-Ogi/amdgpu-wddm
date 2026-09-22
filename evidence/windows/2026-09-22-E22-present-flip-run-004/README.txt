E22 present-to-scanout, 2026-09-22, bc250kmd 0.7.37. Fresh boot 15:13:19. Full table, engines through stage 8, blit and VidPn flip open. No memory-pressure probe.

The seed read the carve-out, not BAR0: `flip target seeded from VRAM physical 0x270000000, 9216000 bytes, first pixel 0x00000000, onto 0x270ACE000`. Dirty rectangles still came from reading 3, 1920x1200, pitch 7680, physical 0x271398000. scanout.png, the half-scale fbdump of HUBP0 at 0x270ACE000, is the same kind of frame as run 003: black, with the overlay's rectangle and the taskbar islands. The picture is not a desktop. 944 hardware vsync acks. Undo returned display-only, stage 61. No TDR. 66-72 C.

ring.txt is the driver ring pulled before the undo.
