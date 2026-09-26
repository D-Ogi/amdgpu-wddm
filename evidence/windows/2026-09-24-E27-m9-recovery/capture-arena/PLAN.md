# M375 - Share prepared capture storage across pending plans

Hypothesis: interleaved captures whose combined aligned storage fits the existing context reservation can build/resume without heap allocation while preserving independent tokens and metadata. Current code makes even a small capture monopolize the21MiB reservation.

Implement aligned first-fit spans tracked by active capture headers under existing PagingBuildLock. No allocator metadata allocation. Detach returns the span for reuse; context drain frees heap plans individually and the arena once. Keep oversized/exhausted fallback honest; this change is not a proof of a global concurrency bound or complete elimination of DDI allocation. Local MS BuildPagingBuffer documents multipass token preservation and Transfer end/start ordering, but inspected TRANSFERVIRTUALFLAGS has only64KB flags, so do not apply the legacy Transfer guarantee to virtual operations without evidence.

Actual KMD routing regression must interleave two captures while forced pool allocation failure is active, preserve both tokens and first metadata, and drain without freeing arena interior pointers. Add span-hole reuse/alignment/exhaustion checks. Existing byte oracles and WDK build must pass. No lab deployment or reboot in this experiment.
