# M417 - SDMA VMID0 indirect execution on unit A

Candidate0.7.128.1,2026-09-24. SYS SHA256
98931C11FAF37DE35A230022839C5B68C69A6A1999CD39438987F2BD20DA4897.
[Plan](PLAN.md), [independent artifact validation](validation.json).

The optional EnableSdmaIbControl runs after engine initialization and before
WDDM publication. Four trials pass in order: direct4096, indirect4096,
indirect65536, direct65536. Fresh fences1/1,2/2,3/3,4/4 arrive; every destination
byte reads0xA5 after distinct CPU source/destination seeds. The same retained
GTT IB at0x564000 holds16DWORD including padding; VMID0 and CSA0. Reported
12us is the existing submit/poll timer, not a full allocation/seed/readback
latency benchmark. These controls establish fetch and execution, not nonzero
VMID translation or a performance advantage over direct-ring packets.

The new command18 uses the unchanged88-byte copy-control ABI; command16 stays
the direct comparison. Both remain disallowed as external escapes under full
WDDM. No concurrent diagnostic bypass of the OS scheduler was added. The IB
is reused only after the owed fence arrives and retained until teardown's
existing retirement path. This trial does not exercise a timeout or prove all
partial-failure retirement cases. Ordinary OS paging still uses its prior path.

Host actual-source startup coordinator20scenarios and AMD packet oracle18474
checks pass, zero failures. WDK26100 KMD and CLI builds return0. CLI's required
stage-table checks pass. Source and artifact hashes are in sources.json.

PnP disabled127, installed128 while disabled, restored the exact M412 D3D
registration/display gates, then enabled once. No OS, DWM or AC restart.
Boot11:44:14 and DWM4448/start13:58:18 persist through15:30:33. Loaded D3D DLL
D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D remains
Mesa main/LLVM23.1.2 CPU llvmpipe. A scanout was captured privately; no new
manual desktop/input acceptance is inferred from DWM responsiveness.

Post-start current-Mesa RADV controls all return native0:8shader hashes equal
CPU, MLP argmax matches, stories15M96tokens with7/7layers and TinyLlama64tokens
with23/23layers equal E14 Linux text after CR normalization. Loader witnesses
identify the accepted M414 ICD. Independent local validation checks the exact
E14 files rather than reusing only the remote success message.
Final graphics2354/2354,paging22318/22318,zero timeouts/refusals,noTDR.
Capture46reserved/0heap,peak1plan/5505328bytes is session evidence, not a bound.
1000MHz/820mV,final66.8C,guard0; test gate cleared, presentation gates retained.

Text copies decode native UTF16 where needed, otherwise UTF8; native newlines
are preserved. PCI instance/interface identity strings alone are redacted.
The first collection failed on the host CP1250 codec; its incomplete directory
is retained with COLLECTION-ERROR.md. This directory is a complete UTF8 copy
of the same trial, with no hardware rerun. Private scanout stays outside repo.

Next: nonzero paging VMID/root/CSA policy and a mapped-address data oracle,
then OS-owned DMA ranges. M9 resource/status/cache/PFN/lifetime and matched
Windows/Linux performance requirements remain open. M417 does not close M9.
