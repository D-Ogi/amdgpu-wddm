M308 - Linear table publication acceptance
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus test modifications, snapshot included.

Four additional owned-transfer fixtures use actual WddmMemoryLayout/VidMmStartLayout and six initialized local table pages. Both one-byte shift directions and alias/disjoint mappings traverse multiple buffers. After each actual packet replay, all3072 logical PTE values match the independently modified bytes, including untouched entries; all live backing entries remain at their CPU initialization values. Physical address capture still uses the synthetic translator, disabled after first callback: this does not prove actual self-mapping linear traversal.

An initial disabled VidMm write gate refuses publication after capture/emission without advancing retained progress or DMA pointers/sizes. Restoring the gate lets the same owned capture finish. Capture/table resource counts drain.

Routing129862checks PASS; focused98836PASS. Generated OmitLogicalCommit focused74235checks/6158failures native1; omission also bypasses initial publication refusal, so subsequent callback counts differ. Test detects missing table publication rather than claiming every failure is independent.

Tests only, no new driver build/deployment. Latest development SYS remains M306952F05B7142D18A4E526622F160F9B8477380EF20E5AD42106D45F567D7FF556. General interval graph, actual self-mapping linear walker, OS ownership/status/cache/initialization and GPU performance remain open. No lab access/mutation this turn; sshd reply pending.
