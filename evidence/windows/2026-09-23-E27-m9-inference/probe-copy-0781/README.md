# Diagnostic IB read without new NC mappings, candidate0781

2026-09-23, base bed764d plus ongoing uncommitted M9 work. No lab access.
VidMmProbeIb now holds the shared CpuUpdateLock and walks the retained table map;
it no longer maps/unmaps each table separately. A system leaf is read through
MmCopyMemory with MM_COPY_MEMORY_PHYSICAL into the existing nonpaged kernel-stack
output buffer. No driver-created NC alias of OS RAM is introduced by this probe.
The request is DWORD aligned, limited to the current4KiB page and240 DWORD output,
requires a bounded system range and excludes local storage. Exact success/byte
count is required; failures and short copies clear metadata and all output words.
Local leaves retain the prior metadata-only behavior. Optional diagnostics remain
controlled by TraceUmdProbes; this does not synchronize the snapshot with GPU work.

8939 host checks pass,17 added. Actual wrapper/walk runs against CPU-initialized
page tables with mocked MmCopyMemory and lock/mapping APIs. Captured physical
address/length/flags, page-end handling, error/short output clearing, unaligned
refusal, post-stop refusal and zero extra mapping calls are checked. No real OS
copy, concurrent StopDevice timing or GPU execution is tested here.

Full0781 WDK build/sign passes. SYS SHA256:
E7BEABA4BD27272D8BBEA5B2F5F6190F60C3D35C7D99D58974231EACEB37A647
Package scratch/build/bc250kmd-0781/package-umd, NOT DEPLOYED.

Primary Microsoft API contract checked2026-09-23:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-mmcopymemory
Physical source must be ordinary OS RAM; I/O-space sources fail. Output must be
nonpageable; IRQL at mostAPC_LEVEL. Caller inspects status and transferred count.
Probe entry remainsPASSIVE_LEVEL. These facts do not establish a consistent GPU
snapshot or OS page pinning beyond the submission/lifetime contract.

Cache investigation also located MmGetCacheAttribute and Ex declarations in WDK
26100 ntddk.h10781/10815 (Ex has MM_GET_CACHE_ATTRIBUTE_IO_SPACE). No runtime use or
claim that these expose a particular existing CPU_VIRTUAL mapping's PAT attributes.
The inspected display docs do not resolve the borrowed mapping cache type. M190
retained table-map/OS alias and application Present aliases remain open, as do
DDI error policy, physical ADL, actual GPU reset/reentry and1GiB/performance gates.
