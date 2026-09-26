# GPU residency small control, unit A, 2026-09-23

Probe SHA256 BE3F5F8C95A70EA40164F7ABC641556870B738B91B577918CEA0457895AB60E4, built /W4 /WX and help smoke passed. Current full WDDM0798; no reinitialization. Source snapshot included. Packet definitions imported verbatim from MIT Linux amdgpu nvd.h; packet order follows MIT Mesa radv_cs_emit_cp_dma, direct memory path with CP_SYNC. No new register writes.

65536-byte VRAM control: initial GPU full-range readback and all3cycles pass. Evict residency2, MakeResident residency1 each time. Sentinel-filled readback destination and every dword checked after monitored fence values1..4. GFX counters22039->22043, all completed; SDMA167240->167366 all completed. Zero timeouts/refusals/TDR. Aggregate transfer calls93882->93894 do not identify the tested allocation's physical relocation, so no per-allocation transfer claim.

Harness limitation: PowerShell Process.ExitCode was empty (vram64k.exit contains no numeric exit), despite the explicit final PASS and all witnessed cycle/fence/readback results. Scale harness captures native exit through cmd.exe. Large1GiB run underway, not established by this small control.
