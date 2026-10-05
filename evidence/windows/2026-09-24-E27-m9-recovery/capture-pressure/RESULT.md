# M382 - Capture pressure instrumentation built, not deployed

Owner state now tracks active unfinished plans, their reserved arena bytes,
and the maximum of each. Attach takes ownership before increasing occupancy;
detach lowers occupancy, and drain clears current values while retaining peaks.
Heap plans contribute to plan count but not reserved arena bytes. These byte
metrics are not total heap demand and do not include fragmentation/largest-hole
information. Adapter summary retains the maximum observed per-context plan count
and per-context reserved-byte count, independently, after context destruction.
They need not come from the same context or instant and are not global sums.

The only writer holds PagingBuildLock. Atomic publication permits concurrent
summary reads. There is no new allocation, GPU command, MMIO or public IOCTL
layout change. A resumed plan does not attach or increment counts again.

Extracted actual builder/ownership tests: 329199 checks, 0 failures. Assertions
cover sequential reuse (one active plan), two interleaved plans with pool
allocation disabled, three-span middle-hole retirement/reuse, duplicate attach,
and drained owner with preserved adapter peaks. Final native exit 0. Existing
packet byte oracles remain passing. Initial passing test log is also preserved.

WDK build and package checks pass; see build.log and package.json. Package
0.7.120.1 SYS SHA256:
F1319389C30C10FB59BD5228780FE5924212DFEF587BA054D2FB519DD7034774.

No deployment or hardware test in M382. Unit A remains on the previously
verified initialized119 session; no reboot, PnP transition or AC action was
performed. Use120 when a driver transition is otherwise needed. These counters
will measure observed construction pressure; they cannot prove a universal
operation bound or remove the existing exhausted/oversized heap fallback.
