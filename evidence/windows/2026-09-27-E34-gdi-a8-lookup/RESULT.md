# A8 lookup-table geometry and review controls

M622, development-host validation only. Exact source hashes are retained.

Local WDK26100 d3dkmdt.md GDISURFACE_LOOKUPTABLE specifies D3DDDIFMT_A8.
The common GDI layout helper now accepts one-byte pixels for type4 and rejects
four-byte lookup pixels. Kernel assertions bind type4 and the maximum type8 to
the WDK enums. This corrects the new validator's rejection of an A8 lookup
allocation. It does not implement RenderKm, lookup initialization or ClearType.

The mandatory geometry test passes58232 checks, zero failures. Added controls
cover type4 A8, current geometry policy for types5-8, misaligned legacy pitch,
large64-bit pitch-height products, unsupported pixel formats, and an independent
Mesa shared-layout loop. Testing geometry for types5-8 does not establish their
ownership, placement or interop support. The Present test now verifies that a
20-DWORD capacity (large enough but unaligned) returns NoSpace without writes.
All12 quick gates and actual WDDM/gfx_blt compilation pass.

Logs are copied unchanged; no private identifiers removed. No deployment or
runtime validation. Local KMD155/19f0dfe predates this correction and must not be
mistaken for a package containing it. CDD identity and runtime integration remain
open; full G0 is not established.
