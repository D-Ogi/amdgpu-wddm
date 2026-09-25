# M488 - Virtual PTE copy construction

The new builder retains source/destination GPU virtual addresses and emits two
staged copies with fence/poll barriers into an owned DMA span. It supplies
disjoint CSA, marker, IB and staging regions without allocating resources.

The host interpreter changes both endpoint mappings after construction, then
checks all bytes against memmove, including distinct-VA physical aliases.
148 relocation/alias cases and 1024 DMA start alignments pass. The combined
suite reports 15048 checks and zero failures, including 10064 prior packet
checks. WDK26100 kernel compilation passes with warnings treated as errors.

Scope: host construction and execution-model evidence only. No KMD was linked
or deployed, and no GPU work or lab restart occurred for these controls.
DDI integration, submission-time root ownership, GPU-ordered relocation,
Windows companion paging and Linux parity remain open.

Reproduce with driver/shim/test/run_paging.ps1 -VirtualPtes, then
experiments/E33-m12-applications/compile_virtual_ptes_kernel.ps1.
The manifest records all archived input hashes and the produced binary hashes.
sources.zip preserves the new code and the shared worktree dependencies used.
host.log and kernel.log are unchanged tool output. The integration plan is
experiments/E33-m12-applications/VIRTUAL-PTE-COPIES.md.
