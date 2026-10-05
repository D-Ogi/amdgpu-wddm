# M421 - Typed native paging records and mixed ring submission

Date2026-09-24. Source/host only; not deployed.

PagingPrivateNativeHeader describes a whole OS DMA range, a pinned system
paging root, aligned IB payload and a disjoint64-byte CSA. Its private record
is64bytes independent of DMA payload size. Existing direct records retain
their24-byte header and copied words. No global buffer identity is introduced.

PagingPrivateVisitMixed validates the entire selected range before callbacks.
Native records require virtual submission and whole-record boundaries; direct
records retain their previous subrange semantics. Legacy direct-only visitors
reject native records rather than treating metadata as SDMA command words.

WddmSubmitPagingHardware now accepts this typed validation. The GfxSubmitPaging
consumer counts actual direct words plus a conservative21+7+6DWORD native
root/TLB/IB cost, reserves once, and emits records in source order followed by
one outer fence. Any emitter failure undoes the unpublished reservation. It
retains the existing DISPATCH_LEVEL doorbell mechanism, queue ownership and
honest completion. The native metadata refers to OS storage that must remain
resident until that submission's real fence; it does not allocate another IB.

Private-data tests pass32mixed direct/native/direct layouts, isolated pending
roots even at equal VAs, exact native selection, disjoint CSA/IB, and rejection
of truncated, overlapping or partial-native selections before any callback.
The actual KMD route/shadow/alias suite still passes329199checks. Full WDK
build/signing passes. These do not test hardware execution of typed records.

The builder STILL emits only direct records. No native OS work is claimed and
capture allocation remains. Next implement ordinary TransferVirtual/FillVirtual
record generation, including bounded ring/DMA/private capacity, valid system
root/CSA placement, ordering and preservation of logical table publication.
Use the M420 aperture-backed DMA storage; leave supported zero-VA/bootstrap
work on its appropriate existing path.

DEV SYS962203BE3D72F49469EFD9F590A762CC6D15A10FE76A613FDA7D2666F0F63B96
uses unchanged130version but is NOT the installed130artifact and was NOT
copied to the lab. Installed SYS remains M420 BB96304B... . Bump candidate
version and validate the complete native builder before deployment.
