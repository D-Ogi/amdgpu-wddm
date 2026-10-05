# M227: virtual Transfer publication, candidate0790

Date: 2026-09-23. Source/host validation; no lab access or deployment.

WddmBuildVirtualTransfer uses the paging-process root, independently bounded
source/destination page slices, captured physical identities, header preflight
and logical table commit before DMA publication and before the next translation.
Table destinations are selected from the actual advertised table extent.
MultipassOffset counts completed slices and represents64-bit byte progress.
The new early DDI arm accounts actual moved bytes and propagates helper status.
Unreachable legacy virtual Fill/Transfer branches were removed; the generic
update/flush branch retains its previous policy. This is not complete DDI
restricted-error compliance.

11746 routing checks pass (+34). Actual VidMmStartLayout, CPU initialization,
logical/live walkers and publication execute in the host harness. A zero source
table is copied over the leaf table containing the mapping for the next slice:
one4096-byte packet is published, logical translation then refuses, and the prefix
remains intact. Live CPU contents remain unchanged until an independent host
memcpy executes the decoded packet. Buffer/header refusal controls preserve the
mapping. A separate two-buffer copy resumes and reaches both independently mapped
destinations. A modeled17-byte tail resumes beyond4GiB. The initial test's13-DWORD
capacity was too small for aligned reservation; corrected to16 DWORDs, which
permits one7-DWORD command and refuses the second.

OmitLogicalCommit control:11745 checks,608 expected failures, including three new
self-table-copy assertions. The one-check count difference from normal also
existed in M224: bypassing not-ready commit changes later dynamic control flow.
Outer DDI branch/counters are compiled/source inspected, not invoked by this host
harness. No GPU execution, OS-delivery or cache/ordering acceptance is claimed.

Full WDK build/sign passes:
P:/bc-250/scratch/build/bc250kmd-0790/package-umd, version0.7.90.1
SYS SHA256:9FD21A200586AC56AD0AEFA1C88A0CBC896579E8B725E79CAA386F158CE18498
Not deployed. Changed virtual Transfer token format requires a fresh device
session, not replacement during an in-flight multipass operation.

Remaining: arbitrary cross-page physical alias dependencies (per-slice staging
does not solve them), unknown-source table knowledge recovery, aperture map/unmap,
restricted error policy, staging retirement/cache/OS lifetime, GPU recovery,
real paging pressure and performance acceptance. Previous failed paths that
silently returned success now propagate an internal error; this exposes the
unresolved DDI failure policy rather than establishing compliance.
