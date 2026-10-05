# Aggregate paging wait revision3, unit A, 2026-09-23

Raw outputs unedited; source snapshot included. Small control passes native exit0.1GiB initial GPU readback and first complete eviction/restoration cycle pass. NOTRESIDENT3 after dirty pressure, MakeResident takes6328ms then residency1; GPU checks all1GiB again with fence2048. Cycle2 map of pressure allocation exceeds remaining5s tool deadline, native exit1. No three-cycle acceptance. This confirms that the former5s re-residency limit was too short for at least this successful6328ms operation, not that arbitrary timeout extensions fix driver stalls.

Revision4 applies a measured60s aggregate deadline to allocation mapping/residency too, restoring the5s single-GPU-fence deadline afterwards. KMD hardware watchdog unchanged. CP DMA direct-memory readback validates all content; does not cover shader L2 cache policy or identify individual physical transfer endpoints.
