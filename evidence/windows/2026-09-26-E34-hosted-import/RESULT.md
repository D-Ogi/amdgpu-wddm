# M541: borrowed runtime allocations rendered by hosted Zink

Run018 passes three exact 64x64 RGBA clear/readbacks on native shared D3D
textures, including the survivor after first-device destruction. Each readback
has zero mismatches among4096 pixels; process exit0. The runtime allocates and
maps each LB7A surface. Zink imports its explicit linear layout and pitch;
RADV borrows the allocation and GPU VA through private ABI3 with matching device
identity. The surface branch does not call Lock2 or retain a CPU surface mapping.
Separate staging readbacks are correctness oracles, not presentation copies.

Run014/015 exposed Windows Zink fallback definitions where both LINEAR and
INVALID were0 under a comment saying they would not be used. The new modifier
import exercises that formerly unused path. Existing Mesa ac_drm_fourcc.h provides
distinct Windows definitions. This result does not establish a regression in
ordinary upstream Windows WGL. The private import also needs an explicit image
layout pNext without advertising external memory export. Run016/017 retained the
assertion showing that missing branch; a local minidump identified setup_image_pnext.
Raw dumps remain outside Git.

Artifact hashes in run018:
- Hosted ICDC798E45E93CA25E50DDE1FCC183E091E4BFD43D2DCDC0D6DC3CAA646A4EAACDE.
- UMD86C655307C2666151BB3FAD6BA09B1A67490FC751C395722D7781021B5AF799C.

runtime-import patches apply after M540. Their manifest binds before/after source
files. runtime-shared-control.cpp changes render targets to native shared textures
and keeps ordinary staging buffers. All eight fast gates pass. The test script
uses rename when restoring the test UMD to avoid overwriting a mapped section.
CPU UMD8279AC7F and registered ICD9C40083C were restored; DWM1052 unchanged.
Temporary per-process WER settings were removed. No restart or firmware change.

This is same-device rendering into runtime-owned shared allocations. Cross-device
OpenSharedResource contents, native Present GPU ordering, small-window output and
GPU DWM remain unproven. No system promotion or G0 acceptance is claimed.
PROVENANCE: Mesa MIT; WDK/SDK10.0.26100 declarations.
