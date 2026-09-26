# M8 regression on KMD0.7.56.1

E27 m8-regression.ps1 on unitA,2026-09-23. Existing M8 RADV ICD, installed KMD0.7.56.1, full WDDM with GpuVa/GpuSubmit, engines brought up through the existing gated sequence; no display flip. Gfx IB fence control succeeds. E14 eight compute workloads, three runs each:0 mismatches, process exit0. Captured temperature stays below85C. Display-only restored and DWM restarted in finally, UnconfirmedStarts0. This verifies these compute workloads on the newer KMD; it does not prove inference, paging under larger working sets or desktop graphics.
