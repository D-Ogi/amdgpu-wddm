# GTT residency controls, unit A, 2026-09-23

Unchanged M257 probe SHA256 E9566E495A4FB5BB0F37B8A2583AB10F573D0F9ECEE2B84D94E264242529E9C3, source captured in ../gpu-residency0798-v4/. Full WDDM0798, no reboot, initialization or driver deployment. Raw outputs unedited; no private device instance identifiers found.1000MHz/VID116 verified, large-run sampled temperature70.9-71.2C.

Heap GTT requested:64KiB and64MiB native exits0, initial plus3post-cycle full GPU readbacks match every word.64KiB departure2(shared),64MiB departure3(NOTRESIDENT), all return to status1 after MakeResident.256GPU copy submissions for the64MiB run; final cumulativeGFX30507/30507,SDMA550298/550298,zero timeouts/refusals/noTDR. validation.json produced by validate() for each explicit name/size using the captured validator.

Aperture map/unmap callback counters remain0 before/after. Thus this is requested-GTT allocation/GPU-content lifecycle coverage; it does not validate legacy aperture callbacks or prove physical endpoint identities. CP DMA direct-memory readback does not prove shader L2 cache policy. Current permanent aperture capacity256MiB motivated64MiB scale, not a claimed maximum GTT allocation size.
