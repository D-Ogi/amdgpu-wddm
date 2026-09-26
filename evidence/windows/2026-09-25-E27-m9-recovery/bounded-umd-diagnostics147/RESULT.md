# M471: bounded UMD diagnostics on unchanged KMD147

2026-09-25, unit A. The M470 trace resolved52.8% of active DWM sampled CPU
stacks to DebugPrintf. It opened/wrote/closed a file per entrypoint during its
initial10000 records and always called OutputDebugString without a debugger.
Only Debug.cpp changed from the E0ACCC cached-shared UMD: full entrypoint
logging now requires BC250_UMD_VERBOSE=1 at process start; renderer identity,
SetError/assertion/HRESULT diagnostics and sampled frame timings remain.
OutputDebugString requires an attached debugger. Build passes.

New UMD SHA256 FF864DB8CBD6512DDD5538941A6332A9F85AF0D6EA287A5C3FC4A2C2C771E6D5;
KMD remains147/SYS5FCB554A..., Mesa f9a2d34a LLVM23.1.2 CPU llvmpipe.
Warm PnP reloads the separate mesa-quiet147-umd path. The first8s post-DWM
check was too early and failed honestly; subsequent read-only inspection
confirms DWM3636 and LogonUI12212 both load the exact new DLL. Windows boot
03:57:55.500+02:00 retained. No cold boot or S4 accompanies this change.

Both compared captures use the same bounded3s direct cursor helper on Winlogon.
Cache-only -> quiet: present starts84->176, render starts145->179,
render median23.849->1.919ms and max48.520->3.689ms;
present median5.531->1.968ms and max59.709->4.028ms;
maximum render-start gap84.759->17.064ms; glitch events83->0.
Both traces have zero lost events/buffers. Logical movements188/188 match in
quiet capture; input queue/retrieve177/177. No former long render/copy stall
appears in this capture. This supports removing synchronous diagnostics from
the normal path, but processes/resources are recreated and animation content
is not a deterministic pixel-equivalent benchmark. No universal FPS claim.

New UMD shared2048pixel checks both directions,307200green pixel readback,
explicit query completion and loaded-DLL hash pass. Present is occluded while
locked, so this control proves data rather than on-monitor visibility.
Owner reported animation stutter on the cache-only predecessor; physical
feedback after this logging change remains pending. No stable S4, soak or
fullM9 acceptance. Primary shadow design was reviewed but not implemented.

Final05:27:04: native1000MHz/VID116,66.375C; sameboot/DWM, flags7,
generation49529103383 epoch5 completed228 age23510ms, guard1. Idle LogonUI
has~30s updates; continuous60s progress for automatic confirmation is not
established. WPR not recording; summary pause absent; own cursor task removed.
Raw ETLs remain private; exported metrics and cleaned deployment logs are here.
