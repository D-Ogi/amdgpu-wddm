# M738 - Native runtime image capture binds differences to rendered pixels

Runtime011 uses runner revision 4fbcd552 and manifest
6985F23E0204FDADEE9C31C954EFDF60A465B0EDCC9E16D9ED0193742F23402F.
The image-capture client is SHA256
84C328CDC5ACB544CAAC09D4BF908EF9FDDCAE9C348D373726E71423259AB06B.
UMD, engine and ICD are unchanged from runtime010. Workload: offscreen
64x64, eight draws, two fill layers, four shader variants, three frames,
zero warmup. The shader scene uses its own 256x256 target.

Both native CPU and GPU clients exit zero with measured results. All six PAM
images reproduce their corresponding JSON checksums after conversion back to
BGRA checksum order. The three pairs repeat runtime010 checksums exactly.
At tolerance zero, draws differs in 13/4096 pixels (maximum RGBA difference
163/86/67/0), fill in 4091/4096 (80/77/83/0), and shaders in 3589/65536
(2/2/2/0). These are measured differences, not a classification of their cause.
The strict scene gate remains failed; no tolerance has been waived.

The next discriminating control is per-app DXVK on this same GPU and ICD,
with matching engine source and the frozen client. Cross-renderer differences
alone do not establish a native UMD defect or conformance success.

Independent supervisor reports failed-restored after 42.5335508 seconds:
CPU verified, baseline restored, postflight verified, process tree closed.
Cleanup succeeded and subsequent Inspect at 13:12:59Z reports task Missing.
No new test is active. CPU171 remains the restored baseline.

Selected JSON omits module paths, environment and timing; synthetic PAMs
contain only benchmark output. Raw material remains in
scratch/m14/runtime011-ops. This is not window Present, no-copy or performance
acceptance, and does not complete M14.
