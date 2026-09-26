# M284: repeated identical page assignments

Date2026-09-23. Base commit bed764da5192be7132d646be0e6331c1edeb30fd; captured working-tree sources. Host-only, no lab operation or deployment.

PagingPageGraphPlan coalesces repeated identical source/destination identity pairs. Each unique copy contributes one pending reader. Otherwise duplicate reader counts could leave a source dependency permanently outstanding. Conflicting sources targeting the same destination remain unsupported; this change makes no assertion about their OS contract. Canonical self-copies emit nothing. Physical normalization and WDDM routing are unchanged.

Standalone tests enumerate69904edge lists of1..4copies over4identities, allowing arbitrary repeated source/destination pairs. An independent consistency check compares the requested initial source for every destination.19072lists are consistent, accepted and match an initial-snapshot byte oracle at fixed budget3 with scratch overwritten between groups. Conflicting cases publish no plan. All280391prior graph cases and46233permutation cases still pass. Initial /W4 /WX compile found a test variable shadowing the global case counter; renamed before final passing build.

Actual-DDI suite14703checksPASS. Four new endpoint combinations use source[A,B,A] and destination[B,A,B], requiring one swap, not duplicated writes. Actual published packets match the full-page initial snapshot and preserve untouched pages, with existing capacity/private/progress checks. This is packet replay, not GPU execution.

Commands: run-plan.cmd; driver/shim/test/run_paging.ps1 -KmdRouting -Out P:/bc-250/scratch/m9/page-copy-duplicates-host; driver/kmd/build.ps1 -Kits P:/bc-250/toolchain/nuget -Out P:/bc-250/scratch/build/page-copy-duplicates-dev. WDK build/sign passes, SYS SHA2569287BC279D7A02EF14DF3BAA181D8C05840A243B782B603D60B0C89DB3F366CD. Revision100 remains development-only. Partial ranges, conflicting destinations, virtual aliases, runtime acceptance and remaining full M9 lifecycle/cache/recovery/performance work remain open.
