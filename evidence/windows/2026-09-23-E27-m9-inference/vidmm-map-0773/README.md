# Retained VidMm segment mapping, local0773

VidMmStart validates page alignment, segment extent, address overflow and SIZE_T
representability, then maps the entire VidMm VRAM segment with READWRITE|NOCACHE
when GPU-VA writes are enabled. Mapping failure returns INSUFFICIENT_RESOURCES
before Ready is published. This maps existing device memory; it does not allocate
another segment of VRAM. Kernel VA/system-PTE cost and memory-type consistency
with user/display views remain runtime validation requirements.

The immediate CPU GPU_PHYSICAL updater now uses SegmentMapping+tableOffset rather
than MmMapIoSpaceEx per update. VidMmTranslate also walks through this mapping
under a shared CpuUpdateLock; the exclusive CPU-update wrapper serializes CPU
PTE writes. VidMmStop exclusively drains these users, closes gates, unmaps once,
and clears the mapping pointer. No new per-call mapping resources on those paths.
GPU PTE ordering still relies on paging dependencies, not on this CPU lock.
The optional diagnostic VidMmProbeIb retains its old short mappings; no performance
claim for that path. Global VidMm state still assumes one adapter and serialized PnP.

WddmStart now returns NTSTATUS and publishes Device->Wddm only after VidMmStart
succeeds. Its allocation or mapping failure propagates to StartDevice, which uses
the existing StopDevice cleanup order before returning the error. No engine RUN
is issued during that initialization sequence. Full StartDevice/StopDevice cleanup
has source/compile review only; host tests exercise actual VidMm lifecycle, not
all subsystems or Windows PnP behavior.

6221host checks PASS. New actual start/stop/walker extraction tests local/system
leaf resolution, invalid directory/root bounds, no map/unmap during walks,
post-stop refusal, start mapping failure, extent refusal and gate-closed behavior.
CPU updater now tested against an emulated retained mapping. Walker entry name is
renamed in the harness to coexist with the old resolver-route mock; body unchanged.
SRW concurrency test still rejects unlocked mutation(4violations) and passes actual
wrapper/stop(0), now checking unmap and pointer clearing after a held update drains.
It does not dynamically exercise a held shared walker during stop; shared/exclusive
lifetime ordering is source-reviewed and modeled separately from kernel execution.

Full WDK build/sign PASS; candidate0.7.73.1 NOT DEPLOYED:
scratch/build/bc250kmd-0773/package-umd.
SYS SHA2566897960553995442D267A0A43B86220590E8873001B2BC63EFD37E8B0CD58888.
Post-build source edit is a comment only. No lab access/reset/agents/USB.

Remaining M9: all OS-facing builder error/unsupported-operation semantics,
bootstrap/inherited GPU state, physical ADL, actual GPU halt/reset/reentry,
OS/GPU memory lifetime/coherency, real1GiB paging and performance gap attribution.
