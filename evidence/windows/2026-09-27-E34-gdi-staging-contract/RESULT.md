# GDI staging allocation contract

M617, development-host validation only. Exact parent and working-source SHA256
are in source-sha256.json. No deployment or interop capability change.

The standard GDI allocation blob now appends a 16-byte GDI1 trailer to the
unchanged 32-byte LB7A version1 prefix. It retains Type and reserved Flags through
CreateAllocation and OpenAllocation. Legacy 32-byte blobs remain accepted;
malformed extension lengths, magic, version, types and reserved fields fail.
The current UMD a0ad8af5 OpenResource accepts at least32 bytes and reads the prefix.
Runtime compatibility with the extended blob remains to be measured.

STAGING_CPUVISIBLE now requests only the aperture segment, Cached=1 and
AccessedPhysically=0, independently for each allocation. It rejects creation if
no aperture is offered. This is a requested policy, not a measurement of actual
physical residency, CPU mapping attributes or cache coherence.

STAGING and STAGING_CPUVISIBLE use four-byte-aligned rows with one byte per A8
pixel or four bytes for the supported 32-bit formats. Other geometry retains the
previous policy. Local WDK26100 references: d3dkmdt.md GDISURFACEDATA and
GDISURFACETYPE STAGING_CPUVISIBLE; PresentationCaps.AlignmentShift is2.

Validation: 10616 host checks pass, including the production trailer parser,
legacy ABI size, malformed trailers, A8/32-bit rows and arithmetic bounds.
The same geometry source compiles with kernel flags. All12 mandatory quick gates
pass. wddm.c separately compiles with the exported real KMD command, exit0.
An earlier host-test build stopped on /WX C4127 from a constant CHECK expression;
the check became a compile-time ABI assertion. The failed output is retained.
Logs are copied unchanged; no private identifiers removed.

Not validated: full package link, extended-blob runtime, effective cache policy,
shared backing identity/lifetime and actual GPU Present. Existing-system-memory,
other GDI types and A8 GPU operations remain incomplete. The GDI interop cap stays
off; this does not establish G0 acceptance. Existing local KMD154 predates this
change and M616.
