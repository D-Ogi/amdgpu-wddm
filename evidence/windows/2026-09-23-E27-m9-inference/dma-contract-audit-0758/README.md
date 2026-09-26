# DMA contract audit and local KMD0758 candidate

Source baseline: bed764d plus E26/E27 working changes. This is a source/host-test audit,
not new lab validation. Installed driver remains0757;0758 has not been deployed.
Microsoft reference: ref/ddi-display/d3dkmddi.md, enriched WDK26100 declarations;
original docs commit7515063cea4c9e98db6a92986c5b4ddb0463fd16.

Local KMD0758 build and signing passed. Signed SYS SHA256:
A385061A77C252AB1C1B3C69E7F4353B728234930A7237BDDA6CDE569DB8589B.
Default TraceUmdProbes=0 skips diagnostic IB/shader CPU reads only. Registry setting1
restores them at device start. QPC summaries expose cumulative UMD-call elapsed time
and diagnostic subset; neither is GPU execution time. Synchronization/flushes unchanged.
Paging summary split into two lines because the old guard-log entry truncated the reasons.

DmaSize accounting fixed: use remaining bytes directly and decrement by emitted bytes.
Actual old/new accounting statements extracted by generate-paging-budget-test.py.
Four cases share a64-byte DMA allocation: initial, appended, exactly full, empty tail.
Old0757 fails3cases;0758 passesall4. Existing six preemption scenarios pass0758.
Build, tests and source snapshot are preserved here; GPU behavior remains unverified.
