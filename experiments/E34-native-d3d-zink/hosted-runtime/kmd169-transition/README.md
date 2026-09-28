# KMD169 bounded transition preparation

Not ready to launch. No lab staging or deployment has taken place.

Candidate: 5985a4164fda6717a19c322f0ca3711a6bad7735, ABI 000700A9.
Rollback: exact KMD166, ABI 000700A6. Package file hashes are pinned in
package-hashes.json. Both arms retain CPU desktop registration and zero Present gates.

phase.ps1 implements capture, disable, install, configure, enable and verification.
It must run under the sibling kmd168-transition bounded-child helper, never directly.
The phase rejects an expired monotonic boundary or a changed boot/host. Candidate
operations stop at 110 seconds; the executable work has a 170-second deadline inside the 180-second task limit.
These checks do not make an uninterruptible kernel operation bounded.

Verification requires the installed SYS, loaded ABI, health, operating clocks,
baseline DLL hashes, registration readback, zero gates and a matching CPU UMD in
every observed DWM. File hashes alone cannot establish a restored desktop.
The verification receipt does not prove visually correct scanout.

test-verify-cpu.ps1 exercises the pure acceptance predicate against a CPU witness
and deliberately incorrect observations. It does not execute PnP or test hardware.

run-arm.ps1 executes the ordered phases through the bounded helper. Each child
gets at most 30 seconds and cannot extend the shared candidate/restore deadline.
Only a typed, successful helper receipt plus a phase completion witness advances
the sequence. A failure with an empty job is distinguished from unknown closure;
the latter must not permit a concurrent rollback installer.

test-child-closure.ps1 covers missing and contradictory closure receipts.
test-arm-helper.ps1 exercises the actual native helper with successful, failed
and hanging child trees; it checks that the timed-out descendant has stopped.
These are host-only controls, not installation or recovery measurements.

launch.ps1 establishes the common QPC boundary before stage validation; watch.ps1 uses it and invokes the native watchdog around
worker.ps1. This outer Job Object includes the worker, phase helpers and all their
children. The watchdog runs independently of the candidate PowerShell process.
Restoration is admitted only after the outer job is confirmed empty. An unknown
closure produces recovery-required rather than starting a competing installer.
A supervisor crash can still prevent automatic restoration; process-tree
termination alone does not restore a driver, and a kernel hang is not recoverable
by this mechanism.

test-supervisor.ps1 uses real nested jobs with a mock restoration operation. It
covers normal closure, pre-mutation cancellation, failure after mutation, a hung
nested tree and a failed rollback. Actual PnP remains untested.

Still required before launch:
- Failure injection through the complete runner, including installer timeout.
- Independent scheduled dispatch and fresh lab preflight.
- A separate bounded GPU trial after the transition has closed successfully.

The transition must not be counted as a successful GPU desktop test. KMD169's
vector ACK fix has passed local build/quality gates and independent review;
its runtime effectiveness remains unmeasured.

## Stage preparation

prepare.py requires committed runner sources, validates both package pin sets,
and creates a fresh local directory. It preserves the sibling script layout,
copies the tested bounded-child executable and emits a SHA-256 manifest.
Pass that manifest digest explicitly to launch.ps1 after validating the copied
entry point on the host/transport side. The manifest is an integrity record,
not a signature or a security boundary against a hostile administrator.

verify-stage.ps1 rejects changed files, changed manifests, empty file sets and
paths outside the stage. The candidate phase still validates the driver package
hashes independently. No network operation occurs during preparation.

The scheduled dispatcher allows one start only. It verifies the action, principal
and three-minute task limit before creating the durable start-request receipt.
Candidate time remains110s; restoration ends at170s from launch entry, leaving10s
before the scheduler limit for startup/closure overhead. This does not guarantee
that Windows can terminate a thread stuck in the kernel. Incomplete closure is
recovery-required, never success. Dispatcher runtime validation remains pending.
