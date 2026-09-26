M304 - Physical proof of monotone unequal-offset copy order
Base revision bed764da5192be7132d646be0e6331c1edeb30fd, working tree snapshots included.

PagingPageAliasDirection verifies injective source and destination normalized identities; every shared identity must have the same logical page slot. Then destination offset greater than source implies backward byte traversal, otherwise forward. This O(Identities+SourceCount+DestinationCount) proof permits fragmented physical pages, disjoint destinations and shifted aliases; it does not infer direction from VA alone. Cyclic/repeated/nonmatching physical identities remain the general interval planner's responsibility. Caller must stage overlapping slices and preserve captured identities across callbacks.

32whole-memory snapshot fixtures cover4source offsets x4destination offsets xalias/disjoint,8192bytes with independent page-bound chunks and per-slice scratch. Routing31002checks PASS including65new checks. WDK build PASS, SYS15707C6F18E82DFD363B647FF90E377B4C447E171A15EC580492CAB53B59CC96. NOT DDI-wired or deployed; M303 opt-in actual builder regression remains unresolved. No GPU/OS or throughput claim.

Both configured lab TCP22 routes again failed this turn. No hardware mutation. Recover07101breadcrumbs after sshd restoration before any new trial. Full goal remains incomplete.
