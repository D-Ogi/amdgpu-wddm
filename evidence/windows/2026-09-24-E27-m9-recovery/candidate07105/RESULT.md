# M336 - Reserved capture plans used during GPU eviction workload

Unit A, 2026-09-24, same Windows boot03:35:18 throughout. Installed and loaded0.7.105.1 (escape0x00070069), SYS40D7E3790C8168A08B5BF21379EDC49D3B6288B477DBBEFFCED422E64C187008. Packagecheck passes25checks/noerrors/nowarnings. Installation with all hardware gates closed returns0 and deviceOK, exact service-image hash, oem79. STOP clear and overlay notified;1000MHz/VID116 verified, pre-install66.4C, pre-workload71.1C and observed workload below85C.

One subsequent full start completes scheduler-read/write/done and the full script; original host session79231 terminal0. No Windows reboot, no AC cycle. This is the first full GPU initialization of the recovered Windows boot, not acceptance of a warm GPU restart.

The unchanged M329 shader/eviction probe and ICD hashes pass. Sixteen baseline rounds and sixteen eviction rounds match the CPU word oracle, three residency departure/restore cycles succeed. Stale-input control returns native1 on round1 after its cycle, matching the expected previous data rather than the new CPU expectation. Native-file independent validation passes. Workload host session66259 terminal0 and scheduled task removed.

New adapter-lifetime counters: reserved plans0->20, heap plans0->0. This is a positive runtime witness for using admitted context storage, closing the missing-use observation in M329. Summary survives ring wrap. Graphics0->34submitted/completed; paging1032->4768submitted/completed; zero timeouts/refusals and no TDR before/after. Hardware content results and capture counts are correlated at workload scope, not a per-plan event-to-fence proof.

This covers three4MiB buffers with explicit diagnostic eviction and does not establish all overlap graphs, PFN relocation, concurrent OS contexts, allocation-free busy/oversize paths, GPU recovery or performance superiority. Warm reentry remains unresolved. The adapter remains running full WDDM105 after the test; display flip/blit gates remain closed. No next reinitialization was performed.
