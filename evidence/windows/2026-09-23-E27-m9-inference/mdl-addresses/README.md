# MDL address adapter

2026-09-23, source base bed764d plus ongoing uncommitted M9 work. Host only.
GfxPagingMdlAddress uses real WDK MmGetMdlByteCount/ByteOffset/PfnArray macros in the
KMD. It validates the described byte interval before calling PagingPageListAddress.
FirstPage selects a PFN; ByteOffset is relative to that PFN's beginning. MDL
ByteOffset describes the valid interval, not an adjustment silently added to every
page. A caller requesting byte0 of a partial first page is refused if the MDL starts
later. This helper does not infer whether such an MDL is delivered by Transfer.

21 new checks verify fragmented PFNs, first-page/progress separation, partial first
and last-page bounds, end exclusivity, page crossing, addition/shift overflow,
malformed/empty/null inputs and cleared output.8960 total packet/routing checks
pass with field-level MDL models. Full dev KMD build/sign validates actual WDK
macros/types. No full DDI, pinning/lifetime or GPU execution acceptance.

Dev SYS97CD405DBA5EF7E36362D9C75CF4E485CD7E858196F63293CDA04ACAB68B6C32
scratch/build/mdl-address-dev retains0781: DO NOT DEPLOY. Official0781 unchanged.
No lab access/reset/deployment. Helper not yet wired to physical Transfer/Fill.

The OS retains ownership of the borrowed MDL and PFN storage; this code does not
lock/free/map the described pages. It is specific to MDLs, not permission to cast
logical ADL addresses to CPU physical addresses. TransferOffset remains a separate
segment-only adjustment in the future DDI adapter; the segment address already
includes base. Next integrate segment bounds, independent endpoints, transfer flags,
command capacity and accepted multipass publication. Review SIZE_T/UINT progress
limits and unsupported operations, plus outstanding M190/cache/GPU recovery gates.
