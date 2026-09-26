# M229: capture and advertise real aperture geometry

2026-09-23. Source/host validation only.

GartCaptureAperture takes GartLock, invokes the existing GartDevice lazy setup
and copies bounded geometry from the AMD device into a pointer-free layout.
No hardware enable or table write is performed. Existing planning setup reads
configuration registers; protocol registers have planning answers. The capture
does not require GART already enabled. Errors clear the output.

WddmStart clears cached geometry each start and captures it before VidMm startup
and WDDM publication whenever a local memory layout exists. Failure frees WDDM
state and returns through existing StartDevice cleanup. QuerySegment4 uses the
captured MC base, size and commit limit, and rejects missing geometry rather than
advertising base0. Display-only startup returns before geometry capture.
A full-WDDM startup advertising local segments now requires available GART setup;
earlier partial bring-up with EnableGart closed is not a supported shortcut.

11839 host checks pass (+7). Tests extract actual capture and descriptor code.
GartDevice setup and fast mutex are modeled: controls verify lock held at access,
balanced release, nonzero base, unavailable/short table, setup failure, cleared
output and descriptor matching. Actual WddmStart/StartDevice cleanup are inspected
and compiled, not executed in this host harness. Initial full compile found the
new declaration before BC250_DEVICE's type definition; it was moved into the
GART declarations section and the full build then passed.

Candidate0.7.91.1, not deployed:
P:/bc-250/scratch/build/bc250kmd-0791/package-umd
SYS SHA256:B191BB0766BF8A4253EEFBA7D2BF0E440ACE380DE69CDCED71A73A6B61B81862

Next map/unmap builder must use the same cached layout and validate it against
the live engine geometry. Required MDL/CacheCoherent/DummyPage handling, ordered
PTE writes/invalidation, hardware/OS lifetime and cache policy remain open.
Legacy CpuVisible/CpuTranslatedAddress aperture policy is unchanged and remains
audited. This change establishes descriptor geometry, not working OS aperture
mapping or hardware acceptance. New geometry requires a fresh device session.
No lab access, reboot or USB change.
