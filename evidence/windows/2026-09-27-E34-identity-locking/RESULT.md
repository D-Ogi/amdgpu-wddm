# M630: locked Present snapshots and executable callback gate

The GPU Present builder now copies both allocation descriptors while holding
one object-list lock. Opened/backing lookup, metadata comparison and duplicate
backing rejection happen inside that critical section. Only value snapshots
escape. CPU object lifetime after unlock no longer controls descriptor access;
VidMm residency and command lifetime remain separate OS responsibilities.

Binding is enabled only by GPU Present or identity diagnostics. Diagnostic
GetHandleData additionally requires PASSIVE_LEVEL; Acquire/Release retains
its APC ceiling. A shared production include supplies both helpers to WDDM
and the host test, avoiding a test-only copy of the logic.

The mandatory allocation-identity gate executes180 assertions with controlled
callbacks and list state: gate/diagnostic budget, IRQL, missing callbacks,
short interface, NULL data with and without token, zero-token successful
acquisition, unlisted/wrong-type/wrong-adapter objects, stopping, unlink before
lookup, clearing at release, duplicate imports, metadata/owner/format mismatch,
and immutable value snapshots. Callback stubs assert no spin lock is held;
metadata comparison asserts the lock is held. This is a sequential ordering
model, not a Windows scheduler, runtime ABI or GPU residency proof.

All13 local gates and actual WDDM compilation pass. An initial harness build
failed /WX because its discarded logging macro left OpenFlags unused; replacing
that macro with a variadic logging stub fixed the harness without suppressing
warnings. Final logs are copied unchanged. No lab change:157 retained; built158
is withheld in favor of a successor containing this hardening. Full G0 open.
