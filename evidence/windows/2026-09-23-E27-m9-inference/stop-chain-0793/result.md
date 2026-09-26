# M233: downstream teardown follows retirement verdicts

2026-09-23. Source/host failure-injection validation only.

Before: GfxStop retained GTT pages when Fini did not establish retirement, but
PspStop could still unload firmware and GartStop could restore translation and
free its dummy page. A sequence fault was also absent from Fini's page-release
condition. A quiet engine with failed undo could clear stages, losing that failure
before a later PnP stop.

Now Fini records GfxStopQuiet from halt, undo and sequence status and uses it for
sequence page release. GfxStop preserves this verdict when state was already
cleared, passes it to memory release, and makes failure sticky. PspStop unloads
only after GFX retirement; unresolved PSP ring/TMR retains its owner and CPU views.
GartStop restores/frees only after GFX, IH and PSP stop verdicts all permit it;
failed restore also retains the dummy page and owner. Second stop cannot clear
the failure. Bc250StartDevice rejects restart of the same device object after an
unconfirmed stop, before starting new hardware work.

Extracted actual Fini/GfxStop/PspStop/GartStop functions execute against modeled
hardware/OS primitives.128 combinations cover GART lookup failure, halt refusal,
undo failure, sequence fault, PSP unload failure, GART restore failure and IH
retirement, plus8 manual-Fini cases.1676 checks pass; pre-change functions fail889.
The positive case releases normally. Real resource allocation and hardware halt
are not exercised. StartDevice's new guard is compiled/source inspected, not
included in the extracted stop harness.

Full WDK build/sign passes:
P:/bc-250/scratch/build/bc250kmd-0793/package-umd, version0.7.93.1
SYS SHA256:497849173ACC1D7031A0DBBB55EE2B92BC029B5F3D31519219F8A49E36553315
Not deployed; no lab access, reboot or USB changes.

Limits: this deliberately retains resources on failed stop; it is not successful
GPU reset/recovery. The restart latch belongs to one device object and does not
survive RemoveDevice/AddDevice or driver unload/reload. System-wide containment,
OS-owned DMA lifetimes, direct diagnostic escape teardown/retry policies and a
proven hardware reset remain unresolved. Do not enable automatic GPU startup on
the strength of these host controls alone.
