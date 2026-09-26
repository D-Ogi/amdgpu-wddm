# M532: native WDDM allocation import into RADV

Run006 passes with exit 0. D3DKMT creates a 64x64 shared linear LB7A allocation
on one device; the app-local RADV opens its NT handle on its own device, clears
it with Vulkan, and the original device reads all 4096 pixels as exact blue.
The caller retains ownership of the NT handle. Resource/device destruction succeeds.

Runs001-003 reject shared allocation creation with E26R non-shared policy.
Run004 is a successful unshared allocation control. Run005 creates the shared
allocation with E26R shared policy but ShareObjects fails. Run006 supplies the
OBJECT_ATTRIBUTES and SHARED_ALLOCATION_ALL_ACCESS required by the local WDK
reference; export, import and GPU access then pass. Both call parameters changed,
so this does not isolate which rejected parameter caused the earlier failure.

The candidate parses LB7A rather than proprietary AMD allocation metadata,
separates OpenResource private-data buffers and destroys imported resource handles.
It also preserves caller ownership of imported Win32 handles as Vulkan specifies.
The incremental source patch and probe are in experiments/E34-native-d3d-zink.
Raw logs are unchanged. No firmware, serial, MAC, SSID or credential data is included.

Limitations: explicitly shareable KMT-created aperture surface, not a runtime D3D
back buffer. CPU fence wait serializes this probe; shared monitored-fence ordering
and DWM presentation remain untested. No system ICD/UMD registration or OS restart.
This is not a claim of GPU desktop completion or full external-memory conformance.

References: local ref/ddi-display/d3dkmthk.md (WDK 10.0.26100), ShareObjects
and OpenResourceFromNtHandle; ref/Vulkan-Docs/chapters/memory.adoc (01aaacd9),
Win32 import ownership. New architecture notes: ref/m13-notes/zink-behind-d3d10umd.md.
