# Bounded GDI allocation-contract diagnostic

Purpose: identify the D3DKMDT_GDISURFACETYPE values requested by dxgkrnl before
implementing the CDD-to-DWM engine blit. The existing generic first-eight-call
log can be exhausted by primary/shadow allocations before any GDI request.

The KMD records the first size-query and private-data-fill request for each
WDK10.0.26100 GDI type0..8. Unknown future values share slot9. An atomic bitmask
limits this to20 records per adapter start, including both phases. Records
contain type, flags, dimensions, format, output pitch and extent. They contain
no pixel data. This does not change allocation flags, the LB7A blob, advertised
CDD/DWM capabilities, rendering or presentation behavior.

Local validation: wddm.c compiles with the exported KMD compile command; the
prototype-warning gate passes its valid, missing-declaration and wrong-arity
controls. Private logs: scratch/g0-hosted/gdi-contract-build. The first gate
invocation lacked the MSVC environment and failed before compilation; the
subsequent check.cmd runs vcvars64 and both checks pass. No diagnostic KMD has
been linked, signed or deployed by this change; no lab result is claimed.

Next measured use: include this diagnostic in a committed/versioned KMD candidate,
verify exact source/artifact identity and existing KMD gates, then collect its
initial GuardLog records during a coordinated lab slot. Compare size/fill types
and geometry with the WDK definitions before enabling CddDwmInterop or selecting
an engine-copy route. A missing line is not proof that a type was never used:
the bounded ring can wrap, so preserve the stream or early snapshots.

Sources: local ref/ddi-display/d3dkmdt.md (D3DKMDT_GDISURFACEDATA and
D3DKMDT_GDISURFACETYPE), WDK/SDK10.0.26100 shared/d3dkmdt.h:1380-1403;
ref/ddi-display/d3dkmddi.md DriverSupportsCddDwmInterop and DxgkDdiPresent.
