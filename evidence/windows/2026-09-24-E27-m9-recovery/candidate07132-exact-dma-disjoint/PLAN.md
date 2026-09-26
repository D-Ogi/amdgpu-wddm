# Exact DMA/data disjointness for native paging

Plan2026-09-24, before candidate132 tests or deployment.

Source finding: WddmNativeDmaMapping rejects a DMA physical span whenever it
intersects the min/max physical bounds of an endpoint. For fragmented backing,
those bounds also include holes occupied by independent allocations. This is
a sufficient disjointness test but not an exact overlap test.

Hypothesis: distinguish actual shared bytes from holes by walking the logical
endpoint pages only when coarse bounds overlap the DMA span. No capture array
or per-call pool allocation is needed. True aliases still use the physical path.
This may explain131's unchanged native transfer count during1GiB residency;
the source defect does not by itself establish that hardware cause.

Procedure: add an actual-walker fixture with system pages below/above a distinct
DMA page, plus an actual shared-page control. Run old source against the new
positive fixture (expected refusal/failing assertion), then corrected source
(expected pass, actual shared-page still refused). Run complete routing suite
and WDK build. Candidate132 must carry a distinct version/hash.

Hardware: preserve131 evidence and M412desktop/M414RADV. STOP/85C,1000MHz/820mV.
One PnP transition, no planned OS/DWM/AC reset. First64MiB residency with full
GPU readback, then current shaders/models and1GiB residency if healthy. Compare
native transfer/fill counts and capture counts before/after each stage. Require
all native exits, full content references and hardware fences/noTDR. If native
large transfers remain absent, retain that result and instrument the actual
admission reason instead of asserting this was its cause.

Result pending.

Build identity before deployment:0.7.132.1 SYS SHA256 E7AF5A02A3DEDA2CAD25E7D6A789FDA3C2406666D537F583D8ACAB14BDA49652.
Old-source fixture875856checks has1expected failure. Corrected, extended
source/destination fixture875868checks has0failures; WDK26100build passes.
