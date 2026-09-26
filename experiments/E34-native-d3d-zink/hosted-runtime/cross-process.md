# Cross-process shared texture control

Hypothesis: the hosted runtime path opens the same allocation on two independent
D3D devices in separate processes and preserves bidirectional GPU writes after
explicit completion, without stale pixels or leaking each reopened resource.

Run the same executable on the CPU baseline first, then on the exact M561 UMD
and hosted ICD through the process-scoped router. DWM stays on the CPU baseline.
Start with a short control before the full 1000 exchanges. Each direction clears
RGBA8 with a unique iteration value and direction byte, copies to staging and
checks every pixel. An EVENT query completes GPU work before the CPU event
hands ownership to the other process. This tests serialized ownership; it does
not claim an asynchronous cross-device GPU fence protocol.

Every100 exchanges close the imported resource, acknowledge its release, destroy
the owner resource, create a different extent and reopen in the child. Verify
descriptors and both process identities. Bound each query and IPC wait, and put
the child in a kill-on-close job. Preserve exit codes, candidate identities and
baseline restoration. Any mismatch, failed HRESULT, timeout or unexpected DWM
change rejects the control. This is a prerequisite for M13.2, not its complete
residency/process-exit stress coverage. Readback copies are intentional test
oracles and do not measure the no-copy presentation path.
