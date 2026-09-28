# E38: registered ICD baseline promoted from 9C40083C to 93B1D1FD

M569, unit A, 2026-09-27 (lab clock 02:05-02:20 local, UTC+2; boot 2026-09-26T18:44:31+02:00 unchanged, no
reset, KMD unchanged, CPU UMD 8279AC7F untouched, DWM 84 untouched, no registry change). Owner's decision
after E37 showed the registered ICD still carried the pre-M496 DXGI lookup.

## Why

The registered Vulkan ICD `C:\BC250\m10\wsi-final\vulkan_radeon.dll` had been 9C40083C since 2026-09-25
07:53 (Mesa f333dd6d). It loads `DXGI.DLL` by module name during physical device enumeration and deadlocks
under an application-local DXVK dxgi.dll (E37 run 004 minidump, M564); the fix M496 (full System32 path)
had only ever been tested as a swapped-in candidate. Every D3D-over-Vulkan test since then ran through
candidate swaps with finally-restoration of the defective file.

## Candidate identity and source comparison

93B1D1FD (24253952 bytes) is the fork branch `amdgpu-wddm/radv-wddm2-kmt-enum` at c34ab7cd, two commits on
the consolidated 940ab0eb (upstream 05e6c962): generated dispatch header macro guards and adapter
enumeration through `D3DKMTEnumAdapters2` instead of DXGI (patches in E37). Build recipe
`tools/build/build-mesa.ps1 -Config radv`, zero warnings (E37).

Before promotion the source of the other live candidate was compared, so that no other agent's ICD work
would be left out of the baseline: `scripts/compare-current-vs-fork.py` diffs the lab ICD tree used for
3508416F (`scratch/m12/mesa-current-src`, working tree on 05e6c962, no file newer than that build) against
the fork tree at 940ab0eb, over the union of files either side changed, CRLF and the double-encoded
copyright sign normalised.

| files compared | identical | different | one side only |
|---|---|---|---|
| 86 | 82 | 3 (licence headers and one ABI version comment only) | 0 |

Both build directories use the same meson options (debugoptimized, O2, amd only, no LLVM). So FAD08ECB
(E36), 3508416F (the hosted candidate) and 940ab0eb are the same ICD code from different trees, and
93B1D1FD is that code plus the two E37 commits. The 10 KB size difference between the builds was not
investigated (probably source paths in assertion strings).

## Control 1: M546 flip regression on the candidate (kmtflip001/)

`scripts/run-kmtflip001.ps1` is the E36 `run-fork001.ps1` (itself run031 of M546) with only the candidate
path, hash and output directory changed: candidate as system ICD and `BC250_HOSTED_ICD`, fork UMD 848BCBF5
in the router's application slot, recorded control E58D2951 and router unchanged, interactive scheduled
task. Result exit 0: create flip S_OK, swap effect 3 with 2 buffers, 120 native hosted Presents, final
readback 0/76800 mismatches, GetDeviceRemovedReason S_OK. The three overlay captures (hashes in
`capture-hashes.json`, images outside Git because of unrelated desktop content) contain 46800/46800 exact
red, blue and green pixels in the M546 ROI (`screen-kmtflip001-analysis.json`). Baselines 9C40083C, 8279AC7F
and application-slot UMD 346A5140 restored and hash checked, no backup left, task unregistered, DWM 84,
Tctl 67.4 before, 68.1 after.

## Promotion (promote001/)

`scripts/promote-icd001.ps1`, 00:19:02-00:19:18 UTC:

1. preconditions: STOP flag clear, registered file 9C40083C, candidate 93B1D1FD, no test task or process,
   Tctl 66.8;
2. `vulkan_radeon.9C40083C.dll` written next to the active file and hash checked (rollback: copy it back);
3. candidate copied over the active file, hash 93B1D1FD confirmed; a second copy kept as
   `vulkan_radeon.93B1D1FD.dll` (restore source for candidate swaps);
4. registry `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` and manifest `radeon_icd.json` 234175BD unchanged;
5. positive control on the promoted file in place: E14 compute smoke through the recorded runner
   (`smoke-config.json`, `smoke/receipt.json`): PASS, 8 tests, 0 mismatches, live module witness 93B1D1FD at
   the registered path, boot unchanged; Tctl 66.9 after; DWM 84.

The ten `vulkan_radeon.dll.dwm*-held-*` files (7A9970CA) in the same directory predate this work and were
left alone.

## State after

Registered ICD 93B1D1FD. `STATE.md` (local) rewritten; the old block moved to its history. Test scripts
that assert the registered hash must expect 93B1D1FD. Nothing else on the lab changed.

## Limits

Qualification is the E14 smoke and the M546 flip regression on the same boot, plus the E37 feature-level
probe; not the DWM probes, not a game, not sparse. The D3DKMT enumeration path was exercised by every
Vulkan instance creation in these controls, but no test enumerates more than one adapter. Rollback is a
file copy, untested in anger.

PROVENANCE: Mesa MIT; controls and runner scripts from E14, E34, E35 and E36; scripts original.
