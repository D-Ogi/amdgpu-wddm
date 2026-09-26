E22 run 002, 2026-09-22 ~06:45: bc250kmd 0.7.22 (4c233e3, .sys sha256 prefix 9d137aa9dc44fec8) display-only on
unit A, owner absent (the monitor was not observed; the picture evidence is the driver's own scanout read-back).
run-002-script.ps1: gates EnableMmio, EnableVram, EnableVramWrite, EnableDcnWrite for one device start; `dcn` dump,
`dcnflip 0x270000000` (no-op flip to the firmware's address), `dcnflip 0x271000000 fill 0xFF2060C0` (CPU fill of a
second surface, then the flip), `fbdump` while HUBP0 pointed there, `dcnflip restore`, `fbdump`, `dcn` dump, gates
closed. run-002-console.txt is the unedited output. run-002-scanout-during-flip-half.png is the fbdump frame taken
while HUBP0's address register read 0x271000000, half scale (pure-Python PNG through mon.py's converter).
