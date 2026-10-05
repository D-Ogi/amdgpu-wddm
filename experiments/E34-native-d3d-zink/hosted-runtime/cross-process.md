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

## Owner exit with an imported reference still live

Optional fourth argument normal or abrupt reverses ownership after the exchange
control: the child creates68x36 and publishes its completed GPU write; the parent
opens and checks it. Then the owner exits normally, or terminates itself with42
after explicit GPU completion. Keep the parent's imported texture live, wait for
the owner's actual process exit, check the retained pixels, perform ten new GPU
writes/readbacks, close the import and check device health. Compare the exact
control on CPU and hosted GPU stacks. The expected forced exit is42 only; other
exit codes, stale pixels, timeouts or device loss reject the control. This tests
owner-process cleanup after completed work, not a deliberately stalled GPU.

## Duplicate imports on three devices

Optional fifth argument duplicate (after normal or abrupt) opens the shared
surface on an additional independent D3D device in the child. At each new
generation, both imports read the parent's completed write. Close the first
import, write through the retained third-device import, reopen the first and
verify all pixels before the parent reads the reply. Closing a generation
releases both imports before the owner. The required duplicate_reopen witness
count is ceil(iterations/100). Run101 exchanges to cover two allocation
generations, comparing baseline and hosted GPU artifacts. This serialized
control does not claim concurrent destruction safety or GPU fence handoff.
