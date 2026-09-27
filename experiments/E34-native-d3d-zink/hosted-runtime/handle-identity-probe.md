# Bounded WDDM2 allocation identity probe

Hypothesis: the WDDM2 AcquireHandleData callback resolves the allocation at open
where GetHandleData returns NULL. Alternative: both return NULL or acquisition
returns an object outside the driver's live allocation list. No identity is
adopted solely to make the GPU Present path proceed.

PROVENANCE: Microsoft graphics-driver-samples, MIT.
Reference: compute-only-sample/coskmd/CosKmdDevice.cpp OpenAllocation uses paired
DxgkCbAcquireHandleData/ReleaseHandleData. Local WDK26100 specifies the callback
ABI and reference lifetime. The diagnostic uses that callback pattern without
copying sample allocation ownership code.

EnableHandleIdentityProbe defaults0 and is independent of EnableGpuPresentBlit.
When enabled at adapter start it reports interface size, header-derived offsets
and callback pointers. At most16 non-BC2A and16 BC2A opens compare GetHandleData with
AcquireHandleData and a live-object lookup. Acquisition requires both callback
pointers, sufficient interface size and IRQL<=APC. A nonzero release token is released even if private data is NULL; a successful
pointer result also always takes the paired release. The token is logged. No acquired identity changes the opened
object or the GPU Present admission decision.

Original diagnostic procedure (runtime result: M627):
1. Build an isolated successor to155 containing the A8 correction and this probe;
   preserve153 rollback artifacts and exact settings. Keep GPU Present and GDI
   interop disabled. Keep the working CPU presentation settings.
2. Enable only the identity probe through the normal bounded in-session candidate
   transition, preserving prior registry values for restoration. Capture callback
   header diagnostics and CPU desktop allocation/open logs.
3. Run one existing bounded native allocation control to obtain BC2A opens and
   one LB7A shared-resource control. Preserve exit status, adapter health, exact
   modules and open logs. Stop on callback failure or device trouble.
4. Compare callback results with live allocation objects. A non-NULL acquire that
   matches a live object supports the hypothesis for that case; NULL or mismatch
   does not. Pointer values alone do not identify their owning kernel image.
5. Before production adoption, test duplicate/shared opens across devices and
   lifetime. If both callbacks fail, add deferred re-query; do not infer backing
   identity from equal geometry or raw per-device handle values.

M627 records the diagnostic deployment and runtime result. This is D0 metadata and D1 acquisition only;
callback image ownership, deferred lookup and independent VA mapping are not
implemented. It neither submits GPU work nor writes pixel data.

## Binding successor (M628, not deployed)

The successor uses Acquire/Release on every valid opened allocation, regardless
of the diagnostic gate. It publishes only a live same-adapter allocation under
the destruction lock and releases before returning. Get remains a bounded
diagnostic comparison. See M628 for host validation and runtime checks still
required; M627 validates the probe, not the production binding successor.
