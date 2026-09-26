M311 - Linear transfer changes its own actual page-table mapping
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus working-tree changes; test snapshot included.

Two additional kind4fixtures use actual production table layout, CPU-initialized root/directories and leaf table page4 mapping source/destination to pages4/5/6. Positive controls walk all six mapped VAs through actual extracted VidMmTranslatePaging. Both byte-shift directions use context-owned captures and actual staged/direct SDMA packet replay across multiple callbacks.

In the source+1/destination-aligned case, the first accepted buffer changes the source VA mapping while status remains insufficient-buffer and capture remains owned. Subsequent construction still emits original captured physical addresses. Every callback compares1536logical entries with independent packet replay and unchanged live CPU backing. Final full-byte snapshot and resource drain pass.

The refusal test first captures with zero DMA capacity while the real walker is enabled, then disables the write gate to test publication only. Disabling it before capture also disables actual walks and does not isolate publication; the fixture was corrected accordingly.

Routing328759checks PASS. Tests only, no new driver binary/deployment; latest development SYS remains M310AA55F016D6AFCCA7DC4DA07C932266F8E942A48AB38437B9750B21072B0A3AFF. No live OS/GPU execution claim. General mixed-direction interval cycles/repeated destinations, OS status/lifetime/cache/initialization and measured performance acceptance remain open. No lab access/mutation this turn, sshd reply pending.
