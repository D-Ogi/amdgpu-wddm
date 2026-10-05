# M628: acquired allocation binding (source and host validation only)

OpenAllocation now resolves backing identity with the WDDM2 Acquire/Release
pair measured in M627. The acquired reference remains held across live-list
lookup and BackingAllocation publication. Both actions share the object-list
lock with destroy-time snapshot clearing. Callbacks run outside the spin lock.
Only a same-adapter live allocation object is accepted. Missing callbacks,
short interface, elevated IRQL, NULL/mismatched data and stopping leave the
new zero-initialized binding unresolved. Existing Present metadata, owner,
format, distinct-backing and BC2C residency guards remain unchanged.

Release occurs before OpenAllocation returns, including nonzero-token/NULL
private-data results. No reference is retained until CloseAllocation. Boolean
bounded diagnostics now fit the record; bound means publication occurred,
not that the reference is still held. GetHandleData is diagnostic-only and
never a fallback. Binding runs with diagnostics disabled as well.

Local WDK26100 d3dkmddi Acquire/Release declarations and lifetime remarks are
the contract; Microsoft graphics-driver-samples MIT, CosKmdDevice.cpp supplies
the existing callback-pair precedent. This does not prove concurrent Present
versus destruction safety beyond the existing OS DDI lifetime assumptions.
No generation stamp or new reference-counted object ownership is introduced.

All12 fast gates pass and actual wddm.c compilation exits0 using the exported
WDK command. These gates do not execute callbacks or kernel lock scheduling.
Logs are copied unchanged. Code is not deployed; lab retains157/9B4AEBED.
Required runtime checks: bound/released boolean witnesses for LB7A and BC2A,
duplicate/shared opens across devices, close/destroy/reopen ordering, existing
copy and shared-surface controls. Only then test gated GPU Present separately.
Full G0 remains open.
