# M685: retained GDI+VA context in DWM038

Archival review only, no new lab operation. The158 startup-log snapshots retained
for M682 contain one distinct guard event300 at0.610s: node0, engine1,
CreateContext flags0x00000006, private data size0. Its immediately following
answer requests4096-byte DMA, segment set2, allocation/patch lists256/0,
NoPatchingRequired caps1. The same event occurs in snapshots003 and004.

The WDK10.0.26100 DXGK_CREATECONTEXTFLAGS layout places SystemContext at bit0,
GdiContext at bit1 and VirtualAddressing at bit2 (local consolidated declaration:
ref/ddi-display/d3dkmddi.md:19935). Thus flags6 names a GDI context with VA.
Exact deployed source933f383 logs the request and answer near the successful
return of Bc250WddmCreateContext (driver/kmd/wddm.c:2572). This establishes a
successful GDI+VA context creation in the captured transition, not just an attempt.

The literal "GDI context" diagnostic at2550 is conditional on GdiContext AND
NOT VirtualAddressing. Its absence is expected here and cannot exclude GDI+VA.
Generic request logging is bounded by WddmFirstCalls, so the retained entries
are not an exhaustive context inventory. No owner PID, RenderGdi invocation,
GPU-backed window redirection or accelerated GDI drawing is proven by this event.

The two excerpt files preserve the exact request/answer bytes, including line
endings. review.json gives original file hashes and line numbers. Full originals
remain under scratch/g0-hosted/dwm038/result. No identifiers needed redaction
from these selected lines. Repetition across snapshots is not counted as another
created context. This refines the backing-choice investigation; G0 stays open.
