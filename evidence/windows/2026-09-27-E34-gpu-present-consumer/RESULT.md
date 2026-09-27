# M612: gated GPU Present submission consumer

SubmitCommandVirtual now recognizes a driver GPU Present record before the UMD
BC2S route. The24-byte BGP1 record binds version, header size, complete IB length
and GPU VA. Nonzero DWORD-aligned VA,32-byte-multiple IB length and nonwrapping
extent are required. A recognized but malformed record cannot fall through to
BC2S or CPU E26P. The consumer requires node0, a live context/root, zero UMD
private-data size, acceptable IRQL and EnableGpuPresentBlit=1 (default0).

Accepted work uses WddmSubmitHardware and the existing GFX completion queue.
No software completion is synthesized for this nonempty IB. Hardware admission
failure follows the existing WddmFailSubmission recovery contract. The new gate
does not advertise CDD/DWM interoperability and is not enabled in the lab.

Host tests check valid binding, truncation, VA/length mismatch, unaligned and
wrapping ranges, all24 single-byte corruptions, and no mutation on build failure.
The existing dirty-list pixel/multipass tests pass in the same mandatory gfx-blt
suite. Host and kernel-flag /W4 /WX compile checks pass; wddm.c and gfx_blt.c also
compile using the exact commands exported by the KMD build script. No full link,
package build, deployment or runtime execution of this consumer was performed.

The Present producer is still missing. It must validate typed allocation handles,
linear format and geometry, physical backing non-aliasing, source visibility,
residency/lifetime and full dirty list; build/pad the IB in OS-owned storage; and
publish the bound private record only after successful emission. Partial records
and mixed BC2S/private-data layouts must never become valid commands. These
requirements, CDD staging and desktop acceptance still block G0 completion.
