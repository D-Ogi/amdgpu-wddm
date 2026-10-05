# M470: CPU-read shared backing and candidate147

2026-09-25, unit A. Explicit E26R v2 CPU-read/non-primary/shared intent requests
Cached system aperture backing. V1 and primary policy retained. Actual-source
CreateAllocation31684 checks pass; primary/WC/reserved mutations fail. Signed
KMD147 SYS5FCB554AE77B04506AA80B4590EE33D7CAA4F8D5A6E720CA89666F736760EC31.
UMD E0ACCCB5591CD2B9C8DC78B83BF1C78DF4F7ECB001D9FA77FE1C323BC48E91CB,
Mesa f9a2d34a with LLVM23.1.2 llvmpipe CPU rendering. New UMD requires147+.

Matched147 single-process4MiB allocation test: v1 Lock2 Protect0x404 WC,
three reads155.853-156.505ms; v2 Protect0x4, reads1.244-1.355ms, about125x at
median. Both full pattern checks and cleanup pass; ordinary cached controls
are around1.24ms in this pair. Earlier146 controls ran at~0.28ms: do not combine
those into the matched speedup. VM flags are not an effective PAT/MTRR proof.

Warm PnP preserves03:57:55.500+02:00 boot. Registry-only change plus DWM restart
initially still loads old DLL; explicit adapter refresh then loads correct new
module in DWM2004 and LogonUI12212. Existing module path/artifacts preserved.
Host-tested M465/M467 and unwired M469 are included; their own runtime acceptance
is not inferred from deployment. Last health flags7/budget1: automatic confirmation
not established in this secure desktop phase.

Content acceptance:64MiB GPU readback/residency control,8 CPU shader hashes and
both E14 exact AI reference outputs pass. D3D shared2048pixels both directions
and307200 green pixels pass with explicit event completion and matching loaded
DLL. Present returns DXGI_STATUS_OCCLUDED (0x087A0001) on locked desktop; this
probe proves content, not visible presentation. Temperature around67C.

New Winlogon ETW:84 presents/3s and83 glitch events; zero lost events/buffers.
Wholetrace maxrender48.520ms and maxPresent59.709ms; prior Default scene differs,
so no causal DWM speedup ratio.52.8% of active DWM CPU samples resolve DebugPrintf;
longest Present has CPU ResourceCopyRegion/memcpy before kernel flip. Owner says
animations still stall cursor. Shared cache improvement is real but not a complete
stutter fix. Next isolate synchronous diagnostics; primary shadow is review only.
No new S4, cold boot, soak, hardware D3D or complete M9 acceptance.

Raw ETL/process logs stay private. Published text normalizedUTF8; lab endpoint,
device-interface/instance and explicit GPU UUID fields redacted where present.
Source snapshot manifest480 files; generated unsigned build helper excluded.
