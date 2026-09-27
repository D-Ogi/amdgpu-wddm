# Validate received surface geometry before allocation and open

M620, host validation only. Source identity is in source-sha256.json.

CreateAllocation and OpenAllocation now validate LB7A version, format-derived
pixel size, dimensions, pitch, allocation extent and page-rounding overflow.
Legacy32-byte surfaces may retain extra pitch/row padding if their complete
logical image fits. GDI1 surfaces must match the exact layout computed from their
type and pixel size. The standard producer uses the same layout function.
A GDI1 tag expresses requested policy, not OS attestation or backing identity.

The mandatory host test reports28040 checks, zero failures. It rejects received
GDI buffers with too-short or too-long size or changed pitch across widths1-129,
heights1-9 and texture/staging types. Legacy padded geometry remains accepted;
bad version/magic, insufficient pitch/size and page-rounding overflow fail.
All12 quick gates and actual WDDM compilation pass. Logs are copied unchanged;
no private identifiers removed. There is no new runtime or deployment evidence.

GDI request logging now occurs before format/type/Flags rejection, bounded to20
lines per adapter start by type and size/fill phase. Unknown types remain refused
rather than given guessed geometry. Reserved Flags still follow the field's
must-be-zero contract; the contradictory flags-structure wording is noted in
review062/064 locally. No interpretation of reserved OS bits was introduced.

Other GDI types, existing-system-memory ownership, actual aperture pressure,
CPU/GPU coherence and authoritative CDD identity remain unvalidated or incomplete.
This change does not enable the interop capability or establish full G0.
