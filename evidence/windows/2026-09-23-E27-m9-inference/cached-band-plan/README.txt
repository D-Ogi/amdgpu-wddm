M301 - Retain current byte-band move plan
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus working tree changes (source snapshots included).

Previously GfxPagingPlanCapturedGraph rebuilt the complete dependency plan for every resumed batch, even when identities and atomic budget were unchanged. Capture now remembers PlannedBand and MoveCount. Preflight leaves its last band cached; later planning rebuilds only on band change and reuses Moves across resumed buffers. No metadata allocation increase beyond two unsigned header fields; per-page storage is unchanged at136bytes. No change to publication cursor ownership or captured physical identities.

Actual-source routing30923checks PASS, including self-table transfer and per-fixture instrumentation asserting at most one planner call per new byte band during emission. Generated ForceGraphReplan restores previous repeated planning:30923checks/14failures, native1, precisely the fourteen planning-count assertions. This demonstrates removed redundant work, not measured GPU throughput or wall-clock latency.

WDK/SDK26100 build PASS. Development SYS F165BB9BBE2CCEDC0F1E5DADE643E3026EC6E8D444D1BB71D09593C8A12BCB48, not deployed. No lab access or hardware mutation. Live OS/GPU acceptance, unequal-offset aliasing, memory-pressure behavior, cache/lifetime/warm-start and Windows-vs-Linux performance remain open.
