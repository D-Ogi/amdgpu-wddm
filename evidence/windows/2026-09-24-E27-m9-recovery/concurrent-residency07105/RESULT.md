# M339 - Two concurrent residency clients

Unit A, same full105/boot03:35:18 as M338. Exact105 SYS/full revision, unchanged M337 probe SHA,1000MHz/VID116 and temperatures<85C verified. STOP clear and overlay notified. Both hidden launchers started24ms apart and were simultaneously live immediately after launch. Original host session25695 terminal0, both nativeexit0; no restart or AC cycle. A later process observation occurred after completion and returned no live probes; it is not a GPU-overlap witness.

Each client uses its own device/context and64MiB allocation, explicit eviction/competing allocation demand, three residency departure/restore cycles and four complete GPU word-oracle readbacks (fences64/128/192/256). Both independent native outputs pass; combined512GPU readback jobs. Final graphics15011/15011 and paging243768/243768,zero timeouts/refusals/noTDR. Reserved plans75->87,heap0->0.

This proves successful completion of two concurrently launched independent clients with overlapping launcher lifetimes; it does not time-resolve simultaneous GPU execution or prove retained multipass-plan interleaving in one system context. The zero heap count is not a bound on possible overlap. OS cancellation/device recreation and all callback interleavings remain unverified. Lab remains full105 in the same boot.

## Resource-contract review

Local Microsoft DxgkDdiBuildPagingBuffer describes stable MultipassOffset and independent submission of partial buffers; the end-before-start guarantee is in the legacy Transfer subsection. Local DXGK_BUILDPAGINGBUFFER_TRANSFERVIRTUAL and DXGK_TRANSFERVIRTUALFLAGS describe paging-process addresses and64KB-page flags, without a corresponding explicit bound on retained interleaving. These inspected texts do not justify removing the busy fallback. Supplemental conceptual examples describe map-source/map-destination/flush/transfer/submit ordering, but not that missing bound:
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/examples
https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process
No allocation-free guarantee or resource design completion is claimed from this review or test.
