E20 runs 003 to 008, unit A, 2026-09-22 (boot 01:40:28), Windows 11, full WDDM table behind its one-shot gate,
EnableGpuVa 1, no engine started in any of them. Target-side ring logs (ring-p<N>-<hhmmss>.log, N = run) are the
driver's own log as pulled after run 008; state/gate/confirm files of each phase alongside.

run 003  bc250kmd 0.7.16 (sys 65cbe991b56dcc83...): OpenAllocation through DxgkCbGetHandleData -> "not one of our
         allocations" for the CDD's handles 0xC00006C0 / 0xC0000000; every blit skipped, source handle 0.
run 005  bc250kmd 0.7.17 (59f74fa29ec3faa0...): OpenAllocation hands out our own opened objects (private blob);
         the 24-byte reading of slot 1 still says handle 0 / 0x8DC000; every blit skipped.
run 007  bc250kmd 0.7.18 (ab33bffb016437db...), EnablePresentBlit 0: the list read as 12 qwords; reading 3
         (32-byte DXGK_PRESENTALLOCATIONINFO, entry 1) names our opened object, 1920x1200 pitch 7680 format 21
         size 0x8CA000 at VA 0x8DC000, translated through the context's root to 0x271398000..0x271C61FFF; entry 2
         is the other opened object at VA 0x12000. No blit (gate closed).
run 008  bc250kmd 0.7.18, EnablePresentBlit 1: every Blt present copied its sub-rectangles into the firmware
         framebuffer (run-008-console.txt). The owner, at the monitor, saw the whole desktop, correct, for the
         minute the full table ran (03:14:50 to 03:15:50). run-008-overlay-screenshot-gdi.png is the overlay's
         GDI CopyFromScreen capture taken at 03:15:01 - black, 3586 bytes: GDI reads the CDD's surfaces, not the
         scanout, so this file shows what the screenshot tool cannot see, not what the monitor showed.

Nothing redacted: no addresses, MACs, serials or SSIDs in these files.
