# Present admission and standard GDI texture layout

The planned arbitrary offscreen D3DKMTPresent control is not supported by the
consulted contract. Local WDK26100 reference d3dkmthk.md, D3DKMT_PRESENT fields
hDestination/hWindow, requires source ownership and appropriate primary surfaces
for nonzero hDestination. An ordinary destination allocation with the current
DWM owning VidPn is not an established path to DxgkDdiPresent. Such rejection
would not validate or invalidate BGP1. No such lab call was made.

The agreed windowed ICD route still requires CDD/DWM interop and its allocation
obligations; the capability remains off until those are implemented and bounded
validation is prepared. An exclusive-primary test would need a separate display
ownership transition and does not replace composition acceptance.

A source-level prerequisite was corrected: standard D3DKMDT_GDISURFACE_TEXTURE
had width*4 pitch and height*pitch bytes. The current UMD OpenResource in
scratch/g0-umd-source (a0ad8af5), src/gallium/frontends/d3d10umd/Resource.cpp,
rejects pitch not divisible by16, pitch less than round_up(width,4)*4, and size
less than pitch*round_up(height,4). Thus many odd-sized standard textures could
not open even before GPU sampling. KMD now uses its256-byte row alignment and
four-row allocation padding for that texture type, keeping logical dimensions.
Other standard surfaces retain their prior geometry. Integer bounds precede
padding. This matches the allocation size convention used by DxgiFns.cpp.

This correction does not implement staging policy, A8 staging interpretation,
existing-system-memory allocations, authoritative shared-open identity, cache
ordering or the complete interop contract. No new capability or lab deployment.
The existing isolated154 candidate predates this correction and is not treated
as a complete CDD/DWM candidate.
