# M395 - Persist existing visibility observations

Keep all invalidation MMIO and original RLC observer reads. Under the already PASSIVE-safe RLC startup scope, persist existing post-request/dummy-read/ACK messages before their RLC reads, then mark observer return. Record GFX return/MMHUB entry, MMHUB return and publication/RLC observation completion. Use a sequence-local flag, cleared after stage call. No extra register access or per-poll snapshots. Run actual-source bootstrap tests and WDK/package checks; no hardware claim.
