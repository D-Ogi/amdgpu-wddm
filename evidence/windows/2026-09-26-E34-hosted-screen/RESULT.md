# M540: persistent hosted Zink screens

Run013 passes the native two-device control with BC250_HOSTED_RENDER=1. Zink
screens now retain their own hosted RADV VkDevice across D3D runtime entries.
Red, blue and survivor green readbacks each contain 4096 correct pixels, with
zero mismatches; process exit is 0. Each screen destroys its hosted paging queue.
No wrong-thread diagnostic occurs. CPU DWM composition remains unchanged.

D3D and DXGI device table entries use scoped, thread-local device ownership.
Hosted callbacks require that scope to match their device. Shader/program jobs
run synchronously during this bring-up; threaded submission is disabled only for
hosted screens. Parallel compilation with runtime-domain allocation marshalling
is still future work, and no compilation-performance claim is made.

Run009 failed before screen creation because vkGetDeviceProcAddr was requested
without an instance. Run010 selected no adapter because consecutive os_get_option
calls reused the Windows option buffer before the LUID was parsed. Run011/012
reached shader creation but hit a util_queue_fence_signal assertion. The private
minidump showed the missing reset in the synchronous job wrapper; no GPU fault
was inferred. The reset was added before execution. Dumps remain outside Git.

Run013 artifact hashes:
- UMD1396540B91F24E46A01E1921923229CBE205B53D1A69D7AE37A4FBB4780A96F7.
- Hosted ICD9ED551B8F90F909D7D1F76A3828A464CFEF2EAA39B2E869F5EC3560A991E29D9.
- ControlD624FAE36995D3F77AD21379261FCE9512838AEEF8BC695A4571B712B9796704.

All eight fast gates pass. persistent-screen.patch applies after M539 and its
manifest records exact input/output files. It is an offscreen control: the
standalone ICD still serves adapter capability screens, while the two rendering
screens use the hosted candidate. Module presence alone is not the evidence for
hosted ownership; the callback and screen-lifetime traces establish that path.

Automatic restoration in run013 stopped on a mapped DLL section. restore013.ps1
renamed the retained file and restored the CPU UMD and registered ICD, checking
both baseline hashes. DWM stayed PID1052. Post-test temperature was66.9C. The
restoration failure is retained in the raw log; process exit0 alone was not
accepted as a completed lab run. Temporary per-process WER settings were removed.

Native runtime-surface import, GPU fence-to-Present ordering, small-window
presentation and GPU DWM remain open. This is not G0 acceptance.
PROVENANCE: Mesa MIT; Microsoft WDK/SDK10.0.26100 declarations.
