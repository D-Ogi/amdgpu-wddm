# M336 - Capture reservation runtime observation on 0.7.105.1

Hypothesis: the existing shader eviction workload exercises successfully attached reserved capture plans; adapter-lifetime summary counters retain that witness after ring wrap.

Unit A: verify pinned SSH identity, current driver hash, boot, closed gates, STOP flag, clock1000MHz/820mV and temperature below85C. Build105 with unchanged UMD stub, validate exact full package before transfer. Preserve pre-install summary. Install with all execution gates closed, without reboot. Verify loaded package version/hash, then run one full startup using the existing104 sequence and firmware identity checks. Never repeat initialization solely because observation times out.

After successful full start, run the same M329 positive16rounds/eviction16rounds/stale-input control with unchanged probe and ICD hashes. Capture summary before and after plus native outputs. Acceptance: content oracles and residency witnesses pass; reserved counter increases; heap counter is reported rather than inferred. A zero reserved delta leaves the hypothesis unproved and calls for a transfer-generating workload. Counter activity alone does not prove GPU correctness or the full allocation-free contract.

Preserve the original process handle and raw stream on interruption. On actual unreachability inspect available state, then use the authorized local plug for recovery if necessary; recover persistent startup before another experiment. No warm restart test is implied. Full M9 and repeated-start acceptance remain separate open gates.
