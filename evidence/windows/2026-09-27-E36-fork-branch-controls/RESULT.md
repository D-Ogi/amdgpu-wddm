# E36: clean builds of the consolidated Mesa fork branches pass the E14 and M546 controls

M552, unit A, 2026-09-27 (lab clock 2026-09-26 23:18-23:22 local). Development PC
builds, no lab build, no promotion.

The two consolidated branches of the Mesa fork (upstream base 05e6c962) were
built from clean worktrees on the development PC with the recorded recipes:
`radv` for `amdgpu-wddm/radv-wddm2-consolidated` 940ab0eb (vulkan_radeon.dll
FAD08ECB, 24252928 bytes) and `zink-umd` for `amdgpu-wddm/d3d10umd-consolidated`
71f2e28c (bc250d3d_zink.dll 848BCBF5, 15105536 bytes). Both builds report zero
warnings (build-radv.log, build-zink-umd.log; recipe-*.json record the tools:
MSVC 19.44.35221, meson 1.12.0, ninja 1.11.1, glslang 16.6.0). The branch
contents are the recorded bc250-win patches through 1f300d3 plus the M546
lifetime patches, verified by manifest hashes in the rework report; they
differ from the lab candidate sources only by comments, SPDX headers, the
UTF-8 copyright sign and two removed llvmpipe test sources.

Control 1, E14 compute smoke (smoke001/): `tools/quality/run-smoke.ps1` with
the smoke007 configuration and the consolidated ICD copied into the registered
ICD path. Result PASS, exit 0: 8 tests, 0 mismatches, live module witness
FAD08ECB at C:/BC250/m10/wsi-final/vulkan_radeon.dll, boot unchanged
(2026-09-26T18:44:31). Baseline 9C40083C restored and hash checked; Tctl 66.2
before, 66.8 after.

Control 2, M546 flip-model regression (flip001/): run031.ps1 with the
consolidated ICD as system ICD and hosted ICD (BC250_HOSTED_ICD) and the
consolidated UMD in the E34 router's application slot; the recorded
runtime-flip-control.exe E58D2951 and router unchanged. Result exit 0: create
flip S_OK, swap effect 3 with 2 buffers, 120 native hosted Presents, final
readback 0/76800 mismatches, GetDeviceRemovedReason S_OK. The three overlay
captures (hashes in capture-hashes.json, images outside Git because of
unrelated desktop content) contain 46800/46800 exact red, blue and green
pixels in the M546 ROI (screen-fork001-analysis.json). Loaded artifacts at the
end of the run: control E58D2951, application-slot UMD 848BCBF5, system ICD
FAD08ECB. CPU UMD 8279AC7F, system ICD 9C40083C and the application-slot UMD
346A5140 restored and hash checked; no backup left behind; DWM 9648 unchanged;
Tctl 68.4 after.

Both controls used the procedures of M538 and M546 with only the artifact
paths changed (the scripts are in this directory). Hosted op counts and
paging create/destroy status 0 in run-fork001.log match run031.

Limits: this qualifies the two clean builds for the bounded E14 smoke and the
M546 flip regression on the same boot, not for the DWM probes (M547/M548) or
for any promotion. STATE.md baseline unchanged. The lab was shared with an
unrelated run032 (M551) of the CreateDevice cleanup work a minute before this
window; both runs restored the same baselines and the final hash check here
is the witness for this one.

PROVENANCE: Mesa MIT; controls and runner scripts from E14, E34 and E35.
