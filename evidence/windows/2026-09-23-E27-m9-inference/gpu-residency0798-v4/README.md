# GPU residency revision4, unit A, 2026-09-23

Probe SHA256 E9566E495A4FB5BB0F37B8A2583AB10F573D0F9ECEE2B84D94E264242529E9C3. Source, packet definitions, harness, raw outputs and validator captured. Build /W4 /WX and help smoke passed. No KMD change, reboot or GPU reinitialization.1000MHz/VID116 verified; large-run sampled temperature69.5-71.5C.

64KiB control and1GiB test both native exit0 and3cycles PASS.1GiB reaches residency3(NOTRESIDENT) each cycle after dirty competing1GiB allocation. After competitor destruction, MakeResident restores residency1, taking6282/6187/6171ms. Full source data is copied by GPU into a sentinel-filled <=1MiB destination and every dword compared. Four complete readbacks (initial plus3cycles),4096actual GFX submissions. Final cumulativeGFX30247/30247,SDMA541409/541409,zero timeouts/refusals/noTDR. validate-gpu-residency.py independently checks log invariants and native exits; validation.json retained.

Changes from earlier harness: pressure precedes departure query; redundant CPU reads of source/pressure removed while every GPU readback check remains; aggregate allocation/map/MakeResident waits60s and measured, per-GPU-fence wait5s and KMD watchdog unchanged. Earlier v2/v3 failures are preserved separately, not erased. The observed6-7s operations explain those specific5s tool expirations.

Scope: positive1GiB OS residency lifecycle and GPU content preservation. CP DMA uses direct memory addresses, so this does not prove shader L2 cache policy. Status/counters do not identify individual physical source/destination pages or establish arbitrary aliases, recovery and other remaining M9 contracts.
