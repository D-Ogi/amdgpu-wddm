E22 run 003 (step 3), 2026-09-22 08:25-08:27: bc250kmd 0.7.24 (85fbf5c) on unit A, owner absent (the monitor was
not observed; the picture evidence is the driver's own scanout read-back).

run-003-script.sh drives e19_target.ps1 -Phase gate -Full 1 -GpuVa 1 -Blit 1 -VidPnFlip 1: the full WDDM table with
the CPU blit of E20 and the new EnableDcnWrite + EnableVidPnFlip, no engines (EnableGart/Psp/Gfx/Ih all closed),
then about 40 s of ordinary desktop, the log ring, and the gates closed again.
run-003-console.txt is the unedited console. run-003-scanout-half.png is `mon.py scanout` (the driver's own fbdump
of what HUBP0 was scanning out) while the flip was live, half scale.
