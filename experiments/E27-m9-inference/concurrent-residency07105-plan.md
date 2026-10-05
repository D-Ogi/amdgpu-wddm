# M339 - Two concurrent paging clients on105

Hypothesis: two independent GPU contexts can preserve data across overlapping explicit eviction/readback workloads with the current capture ownership and paging serialization.

Run two copies of the exact M337 probe, each64MiB VRAM/three cycles, in the existing full105 session. Separate outputs, native exit codes and process IDs; start both before awaiting either. Probe watchdog300s unchanged. Preflight105 hash/full revision, STOP,1000MHz/820mV and temperature<85C. Monitor both original process handles. No restart or duplicate run on observation timeout.

Acceptance: both nativeexit0; four full64MiB word-oracle readbacks each, three departure/restore cycles each; before/after summaries show honest completed counters, no refusals/timeouts/TDR. Confirm process lifetimes overlap. Record capture reserved/heap deltas without assuming that two user contexts imply two simultaneous retained captures in one system context. This is bounded concurrent-client acceptance, not proof of every kernel callback interleaving/cancellation or an upper bound on resources.

Local DDI review: TransferVirtual flags only specify source/destination64KB page mappings. Legacy Transfer's start/end guarantee alone does not prove VirtualTransfer multipass serialization. Keep the busy fallback until a design or contract supplies the missing bound.
