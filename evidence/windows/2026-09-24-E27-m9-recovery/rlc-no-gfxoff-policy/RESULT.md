# M403 - Full WDDM selects the original AMD no-GFXOFF RLC policy

PROVENANCE: AMD Linux amdgpu, MIT; exact6.18.52 gfx_v10_0 helpers retained.
PP_GFXOFF_MASK extracted from localv6.18amd_shared.h. Test uses original GC10.1
register offset and field headers, not hand-computed BAR addresses.

KMD SetUp previously forced pp_gfxoff=true to reproduce E03. Candidate127 sets
it false for Device->FullWddm and preserves true for diagnostic trace replay.
The device comes from the sequence initialized by SequenceBegin before the
only SetUp call. A log records the selected policy. This invokes the existing
AMD-derived rlc_start branch and handshake control helper: RLC_PG_CNTL bit23
suppresses RLC-SMU messages under the documented false policy, then RLC starts
with the original delay. No new reset, SMU command or visibility omission.
This is an explicit full-WDDM power policy while its GFXOFF lifecycle remains
unimplemented, not proof of the cause of M402 or actual power residency.

The new host harness extracts the actual shim and original AMD handshake/start
functions, then compares complete read/write/delay events and final register
values for both policies and two initial states (zero/nonzero unrelated bits).
All4scenarios pass. Reversing the shim policy conditional compiles and fails
all4scenarios (3vs5accesses). Initial harness compilation failed because its
stdint.h include was missing; that log is preserved, corrected final pass is
separate. This negative control models branch choice, not hardware firmware.

Full GFX replay keeps the original true-policy oracle:354+35writes match,
all4historical negative controls discriminate, reset/flush models pass and
WDKshim compile passes. The false-policy mismatch against the old trace is
expected; the separate source oracle above validates that policy's accesses.
KMD WDK build and package25checks pass,0errors0warnings13notes. Existing stack
advisories remain in the build log. No full M9 acceptance or runtime claim.

Package0.7.127.1, SYS
476858800F28A5C103E43FECCC8595C6042BA2479EE18FCFCC3D9FD313953E90.
Not deployed. Lab remains last-inspected126display-onlyboot11:44:14,
USBloaderOFF,plugON. Next first-load GPU oracle and verify pp_gfxoff0, preserve
stop evidence, then one warm trial with the RLC/GRBM checkpoints. If successful,
repeat the GPU content oracle after warm startup before claiming recovery.
