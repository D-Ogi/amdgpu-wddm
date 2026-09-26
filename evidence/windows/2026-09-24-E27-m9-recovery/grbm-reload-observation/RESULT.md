# M401 - Capture GRBM_STATUS around firmware reload

PROVENANCE: AMD amdgpu gfx_v10_0.c/amdgpu_psp.c, MIT; exact local Linux6.18.52
source snapshots retained. Windows PSP11.0.8 import also retained.

The reviewed non-autoload PSP load loop sends per-IP firmware commands without
a separate SDMA preparation callback at that boundary. PSP11.0.8's selected
function table exposes ring operations, not optional bootloader hooks. No
missing pre-SDMA command was established in these reviewed paths. This is not
a complete firmware correctness proof or justification to skip loading.

The GFX soft-reset callback reads GRBM_STATUS to classify CP/GFX busy domains,
then GRBM_STATUS2 for RLC. Current reload logs observe only the latter and
RLC_CNTL. A wider reset cannot be inferred from RLCbusy alone. M351's warning
about actual full-reset dispatch and absent MODE1 callbacks still applies;
this review does not claim that Linux uses or succeeds with soft reset here.
M39 also is not a valid general reload control: M42 exposed stale KIQ fetches
when backing addresses changed. Preserve that limitation when seeking controls.

Candidate126 extends GfxTraceRlcState with one raw GRBM_STATUS read and a
separate log line containing phase,value,readstatus. Register comes from the
existing generated constant and read allow-list; regcalc confirms its address.
No write/reset/policy change, no new MMIO inside the TLB observer. The separate
line preserves the original RLC fields without extending its formatted length.
This adds read latency and can perturb a trial; it is not a readiness verdict.

WDK build passes and package25checks pass,0errors0warnings13notes. No bespoke
behavioral test is claimed for this diagnostic-only change. Source review
confirms the in-flight callback remains free of GfxTraceRlcState calls.
SYS EA573E60DABE3B295D0F9A226A11E0AA07DFD346608F29B371EEC09F3FB30CF5,
package0.7.126.1. Not deployed. Lab remains last-inspected125display-only,
boot11:30:53,USBloaderOFF,plugON. No hardware actions in this review.

Next first-load positive content control plus GRBM samples, preserve full stop
samples, then one changed warm trial. Compare phase-by-phase GRBM_STATUS using
original AMD masks before choosing a reset-domain hypothesis. Ideal M9 and
warm recovery remain open.
