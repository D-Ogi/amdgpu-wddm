# Address-dependent copy ranges with actual logical page walks

2026-09-23, host verification on source0777, base bed764d plus uncommitted M9 work.
No KMD source change, package build, lab access or deployment in this experiment.
Procedure was added to E27 README before running the acceptance test.

Actual VidMmStart and CPU_VIRTUAL initialization register a four-level hierarchy
and source/destination table pages.64KiB-aligned VAs10000/20000/30000 initially map
to physical pages4/5/6. The first copy moves page5 entry0 (mapping page7) into
page4 entry48, changing VA30000. The second range copies from VA30000 into page5
entry1. Actual WddmBuildPagingCopies, publication, commit and logical page walker
must encode physical page7 as the second command's source. The test checks this
address independently, plus the destination logical entry and unchanged live RAM.
A separate host data interpreter decodes copy addresses and applies the two staged
copies; the retained live walker then agrees with the logical view.

run_paging.ps1 -KmdRouting:8676 checks,0 failures, exit0. The new case adds22 checks.
-OmitLogicalCommit:8676 checks,592 failures, exit1. Four additional failures are
specific to this scenario: second source address, logical remap, copied metadata,
and data after modeled execution. Existing588 negative failures remain.

The harness extracts actual KMD functions, with field-level WDK models and host
mapping/allocator/lock stand-ins. This test now uses the actual logical walker,
not the mock translation used in M198's40-range case. It does not execute the
outer OS DDI dispatcher, GPU barriers, TLBs or concurrent callbacks. No hardware
coherency, OS serialization or lifecycle/reset acceptance is claimed.

Candidate0777 and installed display-only0773 remain unchanged. Next integration
gates include cache-alias policy before fullWDDM tests, restricted DDI failure
semantics, physical ADL, actual GPU retirement/reentry/reset and1GiB/performance.
