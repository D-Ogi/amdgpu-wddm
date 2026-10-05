# M262: disjoint indirect physical transfers (host/build only)

Date: 2026-09-23. Base commit: bed764da5192be7132d646be0e6331c1edeb30fd, with uncommitted source captured under source/.

The KMD now admits multi-page transfers between distinct MDLs, aperture ranges and MDL/aperture endpoints when their actual physical byte intervals are disjoint. A source interval heap sort and merge, followed by destination binary searches, avoids quadratic page comparisons. Actual cross-page aliases remain refused before packet publication.

Validation:
- Portable module /W4 /WX: 5000 independent overlap-oracle cases pass, including fragmented/duplicated pages and partial boundaries.
- A 1 GiB disjoint classification makes 524288 resolver calls and takes 0.011 seconds on the development host. This is not a GPU or kernel benchmark.
- Extracted actual KMD routing: 13722 checks, zero failures. Disjoint two-page MDLs, aperture/aperture and aperture/MDL transfers succeed; swapped physical pages forming an alias cycle are refused.
- Signed WDK DEV build passes. SYS SHA256 F2205721605DA46E7BEFC23F663F2CA20F748368550A99AF844FCE9CB422D0C5, 309136 bytes. Existing version retained for local development only; not deployed.

Workspace is paged CPU memory, freed before return, about 4 MiB for an aligned 1 GiB source. Classification repeats on multipass DDI calls; runtime cost and resource policy remain unprofiled. This establishes neither arbitrary alias scheduling nor pending-buffer ownership/cancellation, shader-cache visibility, or live legacy aperture callbacks. M259-M262 changes remain local. Existing installed 0.7.98.1 is unchanged.

Raw logs and source snapshots are immutable. integration.diff isolates the three previously edited KMD files against their pre-M262 snapshots; it is not the complete cumulative repository patch.
