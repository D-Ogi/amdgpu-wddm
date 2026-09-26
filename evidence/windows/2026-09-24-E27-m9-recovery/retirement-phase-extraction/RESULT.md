# M346 - Hardware halt extracted without releasing GFX ownership

Current source has HaltEngines and ReleaseStoppedStorage helpers; Fini invokes
both in the original order. Hardware halt leaves Gfx, SetUp, StagesDone and all
storage intact. The later helper retains the original teardown/unbind/TLB and
GpuMemRelease behavior, including complete halt/undo/sequence verdicts. The
halt-register boolean remains distinct from successful undo; no busy-bit-only
retirement criterion is introduced.

The actual-source stop-chain fixture now extracts those helpers and checks the
ownership boundary.1700checks pass, including24new observations; the historical
negative control remains1676checks/889expected failures. WDK26100 build passes.
The fixture mocks MMIO and actual allocation release, so it proves control-flow
and ownership obligations only, not GPU quiescence or Windows callback ordering.

Local build SYS1F4B491071309747DB01C1FCAE8F0B427584BB3D8C5EFDBCD298728C5ACF03D9
is NOT deployed. Its metadata still says108; do not confuse it with deployed
M344/SYSF0F2AAE21FFF726E86B4262B552CD39CE9BC254BB039CEF796253D5FC2901775.
Assign the next candidate version only after the complete integration is ready.
No hardware action, power transition or OS switch occurred in M346.

This is preparation for M345's required phase separation, not its completion.
Both Bc250StopDevice and StartupPrepare's Unwind still call the combined GfxStop
before PspStop/GartStop. The next change must retain GFX and GpuMem ownership
through hardware retirement and translation/cache retirement, then release it;
manual diagnostic Fini must remain valid while IH/GART can still be live.
GartStop currently destroys its owner/adev and restores firmware mappings, so it
cannot simply move before GFX teardown, which still needs that adev and table.
A separate GART hardware-retirement operation is needed before its final destroy.
No warm-start fix or broad M9 completion is claimed.
