# M235: prepare firmware before hardware activation

2026-09-23. Source/host only.

PspPrepareFirmware reads the existing firmware set and runs the existing LayOut
validation with no staging destination before hardware activation. A successful
opaque owner retains the exact file buffers, device identity, MC base and VRAM
length. Failure releases partial files and owner; output ownership remains NULL.

PspInitializePrepared borrows this owner through the existing PSP execution body.
It checks owner/geometry, skips file re-open and leaves ownership with the caller
even on hardware failure. PspReleaseFirmware releases all buffers explicitly.
Diagnostic and ordinary initialization still use their private read/free path.
The preparation API performs no GART setup or hardware writes; GART may then be
enabled before loading the retained firmware.

32 controls pass: each owner/file allocation failure, each missing file, invalid
layout, stable bytes despite changed modeled disk, failed load ownership, wrong
owner/geometry, quarantine, IRQL, null inputs and legacy read/free behavior.
Actual FreeFiles, ReadFiles, preparation/release/initialization and the execution
file-selection block are extracted. File I/O, layout parsing and hardware loading
are modeled; this does not prove firmware-file correctness or PSP behavior.
The shared-initializer suite still passes141 checks and its mutation fails30.
The M234 unchanged-body comparison is now optional (--verify-original-bodies),
because this intentional change adds borrowing to the PSP core; its historical
M234 comparison remains preserved in that evidence directory.

Full WDK development build/sign passes, retained0793 version:
P:/bc-250/scratch/build/firmware-preflight-dev/package-umd
SYS SHA256:6811CCD4D9BF69E6BEEABAEAA28D1C5CC1B51CB0758AC423BC3F68C1812F450C
Development-only, not official0793; not deployed.

Next allocate all startup reports, call preparation before GART enable, retain
firmware through PSP execution/unwind and release it on every coordinator path.
Partial hardware ownership, actual paging readiness and WDDM publication remain
separate obligations. No coordinator/automatic startup connected; no lab access.
