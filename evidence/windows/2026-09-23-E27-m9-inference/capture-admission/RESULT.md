# M324 - Prepare capture ownership before object publication

2026-09-23, host-only source verification, not deployed.

M323 reserved capture storage after WddmNewObject had already inserted the object
on the adapter list. The new WddmNewContext prepares the reservation first;
WddmNewObjectPrepared copies it before list insertion. Under the existing list
spin lock it either refuses stopping admission, leaving ownership to the caller,
or clears the caller owner and publishes the prepared object. The caller drains
its remaining owner after either outcome. Non-context callers keep WddmNewObject
as a wrapper with no reservation. Bc250WddmCreateContext uses the new helper.

Actual helpers are extracted into the host fixture. An observer at list insertion
verifies the system reservation already exists. Normal context does not reserve.
Reservation allocation refusal, object allocation refusal and stopping admission
leave no publication or live allocation. Existing captured-transfer and byte
oracles still run: 329141 checks, zero failures. Initial host compilation failed
on a redundant SAL macro; removed only that test macro. Both logs preserved.
WDK26100 build passes; SYS 0ADE7E3918E4A6BFF75E11DC9F2321125EBAE9876A508E47A431DAC41EAF29B4, development revision102, NOT installed. Scoped
wddm.c whitespace check passes.

This proves the preparation/ownership order in the tested source paths, not an
observed Windows race or complete CreateContext/StopDevice concurrency. The
adapter lifetime across all external DDIs remains an OS acceptance requirement.
Busy/oversized capture fallback still allocates; full paging resource/status,
cache/PFN ownership and warm hardware reentry remain open. Local d3dkmddi.md
BuildPagingBuffer text only gives TransferStart/End sequencing for Transfer;
no new guarantee was inferred for VirtualTransfer. No online lookup or lab action.
