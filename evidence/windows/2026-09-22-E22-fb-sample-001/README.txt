Framebuffer sample at display-only device start, 2026-09-22, bc250kmd 0.7.38. No flip, no engines.

The new driver mapped the firmware framebuffer at 0xC0000000 (BAR0) and read the first pixel and the centre pixel. Both were 0x00000000. The VRAM-physical sample did not run: the running driver reported the VRAM gate closed. Stage 61 followed, so presents resumed and the desktop came back through the display-only path. No TDR.
