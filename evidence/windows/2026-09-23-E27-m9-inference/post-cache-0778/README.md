# POST diagnostic cache policy, candidate0778

2026-09-23, source base bed764d plus ongoing uncommitted work. No lab access.
DisplayMapFramebuffer now records the successful WC or fallback NC attribute;
failure and unmap clear it. VramMappingProtection compares CPU physical page
intervals, including partially used POST boundary pages. Matching ranges use the
recorded attribute; disjoint ranges retain NC. Mixed ranges extending beyond POST
pages and invalid extents/attributes return0, requiring callers to split/refuse.
Vram Access and DisplayLogFramebufferSample now use this policy; zero protection
is rejected before MmMapIoSpaceEx. BAR0 and carve-out physical addresses are not
conflated. No blanket change to registers or system-memory cache attributes.

Actual map/unmap/policy functions are extracted into a host harness with mocked
MmMapIoSpaceEx.2066 checks pass, including WC, NC fallback, failed allocation,
boundary-page intervals, disjoint physical windows and cleanup. An always-NC
mutation compiles and fails2048 checks. Initial harness symbol-shadow warning
was corrected by renaming its global buffer. Test procedure preceded execution.
The two mapping callers are source/WDK-compiled, not executed by this harness;
actual cache/PAT behavior and PnP serialization remain hardware/OS assumptions.

Full WDK build/sign passes. Final SYS SHA256:
5B3A86C8DF892F880EB2A11DDCAF7FB4AA98E5189CA2A321AC9FECB3483D2EB1
Package scratch/build/bc250kmd-0778/package-umd, NOT deployed. Earlier local build
hashes from this turn were superseded before evidence capture.

This addresses the M200 POST diagnostic mismatch, not all VRAM aliases. Present,
DCN and other mapping paths still need centralized ownership policy; M190 VidMm
OS-owned attributes remain unresolved. No claim of fixing observed black screens,
GPU coherency, DDI failure contract, recovery/reentry or full M9 acceptance.
