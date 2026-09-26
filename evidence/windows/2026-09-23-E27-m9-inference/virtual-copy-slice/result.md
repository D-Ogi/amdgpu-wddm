# M226: virtual copy slice identities

Date: 2026-09-23. Source and host validation only.

PagingBuildCopyPageCore now shares physical and virtual slice emission. Virtual
addresses are resolved once each under the engine lifetime lock; successful
output includes the exact CPU physical identities and system flags corresponding
to emitted packets. The existing overlap, mapped staging, forced snapshot,
capacity and publication-identity rules apply to both entry points. Refusals
clear identities and written count. Virtual addresses require a nonzero root,
nonwrapping extents and a slice within each endpoint's page.

Host routing: 11712 checks, zero failures (12 more than M224). Controls cover
different virtual pages aliasing local or system memory, direct disjoint copy,
forced snapshot, packet/identity agreement, insufficient capacity, missing root,
source/destination page bounds, address wrap and unresolved translation. Virtual
translations in these new controls use the host model; no new actual-walker
dependency or GPU timing claim.

Full WDK build/sign passes. Development-only package retains version0789:
P:/bc-250/scratch/build/virtual-copy-slice-dev/package-umd
SYS SHA256: 3ED64244387FD8A9A2C6B78C16F5DA693C73AC7EFDCF551B3F81D49387FA1B74
Do not deploy this development package as the official0789 artifact.

Not yet integrated with virtual Transfer DDI publication. Next use the captured
slice for table-shadow commit before publication and before resolving another
slice, with wide multipass progress. Whole-transfer arbitrary alias dependencies,
restricted errors, resource lifetime/cache, recovery and hardware/performance
acceptance remain open. No lab access, reboot, USB change or deployment.
