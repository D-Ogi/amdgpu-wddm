# M614: GPU Present producer compiles and links behind a closed gate

Source8f66f9c5ffc9fde6d06a641e931315c01cf57f1a adds the typed LB7A producer to
DxgkDdiPresent. It requires same-device opened objects and verified live backing
allocation identities, equal supported linear formats, checked GPU ranges and
full dirty-list validation. Distinct backing objects establish the non-aliasing
condition for this driver's VidMm-owned allocations; unknown GetHandleData
results remain rejected rather than guessed. BC2A surface metadata is not
supported by this producer. Both allocations must have usable GPU VAs.

The producer builds a complete aligned OS DMA buffer, pads it with the original
CP NOP encoding and publishes its BGP1 private record only after construction.
MultipassOffset carries the packet ordinal. UMD contexts request allocation-list
space when EnableGpuPresentBlit is enabled. The gate remains default0 and no
CDD/DWM interop capability is advertised. See engine-present-producer.md for
outstanding callback, visibility and lifecycle validation.

All11 mandatory quick gates pass, including the existing dirty-list pixel oracle,
record mutation tests and host/kernel /W4 /WX checks. Full KMD compile, link,
stack budget, catalog generation and signing complete successfully. Exact source
capture is clean/eligible and unchanged through the build. Signed SYS SHA256:
FB57329568630D41389F1E7688BD4C2C8C3A362C775387B602879942CC02FC18.
The source/artifact manifest and unmodified build/quality logs are attached.

This is a main-tree development build, not a lab candidate or promotion. The
main tree also contains runtime changes absent from deployed KMD153. No driver
was copied to the lab, no gate was enabled, and neither this producer nor BGP1
submission has run on hardware. A diagnostic deployment must first reconcile
an isolated source tree with the exact153 baseline and validate that artifact.
CDD staging and unknown allocation identity cases remain unsupported; GPU copy
primitive tests M609/M613 do not establish full Present or G0 acceptance.
