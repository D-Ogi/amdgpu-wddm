M302 - Reuse capture-only storage for planner workspace
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus working tree modifications; snapshots included.

Normalization source/destination arrays and identity-sort workspace are dead before graph preflight. They now share a56bytes/page region with the later planner arrays and cached moves. Persistent normalized physical pages, source/destination indexes and domain flags occupy a separate28bytes/page region. Allocation sizes derive from sizeof, with max(capture temporaries, planning workspace). No live persistent array aliases scratch. Probe selection completes before preflight overwrites temporary source addresses.

Actual allocation instrumentation in14host fixtures observes header+84bytes/page, previously header+136bytes/page. Per-page reduction52/136=38.235%. For262144pages (1GiB aligned transfer), metadata changes34MiB to21MiB, excluding header; this is arithmetic from measured allocation layout, not a large-transfer runtime benchmark.

Routing30937checks PASS including complete/partial, local/mixed/system, changed virtual mappings, self-table multi-buffer copies, exact private coverage and plan-call bounds. WDK26100build PASS. Development SYS909F80ED9B9CDF3268FDE5A6E0258EF209EBD1ABEF9ED128AB083A0E01B94367, not installed. No GPU throughput or OS memory-pressure claim. No lab access/mutation this turn. Remaining M9 requirements include unequal-offset dependencies, OS lifetimes/status policy, cache, warm initialization and measured Windows-vs-Linux acceptance.
