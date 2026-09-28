# KMD169 bounded transition preparation

Not ready to launch. No lab staging or deployment has taken place.

Candidate: 5985a4164fda6717a19c322f0ca3711a6bad7735, ABI 000700A9.
Rollback: exact KMD166, ABI 000700A6. Package file hashes are pinned in
package-hashes.json. Both arms retain CPU desktop registration and zero Present gates.

phase.ps1 implements capture, disable, install, configure, enable and verification.
It must run under the sibling kmd168-transition bounded-child helper, never directly.
The phase rejects an expired monotonic boundary or a changed boot/host. Candidate
operations stop at 110 seconds; the whole transition has a 180-second deadline.
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

Still required before launch:
- A supervisor and independent watchdog sharing one monotonic deadline.
- Proven termination of the candidate process tree before restoration begins.
- Failure injection through the complete runner, including installer timeout.
- Exact staging manifest, dispatch and fresh lab preflight.
- A separate bounded GPU trial after the transition has closed successfully.

The transition must not be counted as a successful GPU desktop test. KMD169's
vector ACK fix has passed local build/quality gates and independent review;
its runtime effectiveness remains unmeasured.
