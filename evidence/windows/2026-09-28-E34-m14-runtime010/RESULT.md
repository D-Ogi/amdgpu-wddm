# M737 - Three native scenes execute, but CPU/GPU content differs

Runtime010 source8c28974d, manifestFDBCC549156BE1BBCB073981C88E36F46D2C2D0A7595CFBF3268824A41A8678A. New shell contains SampleCount1 contract correction, same engine/ICD/client as009. Expanded64x64 offscreen workload:8 draws,2 fill layers,4 shader variants,3 frames,zero warmup. Exact scene-set/API/query/settings/checksum gate tested with negative fixtures.

CPU and GPU both return measured/exit0 and complete all three scenes; zero frame-scene API/query failures/disjoint results. Checksums differ for draws04a2bda0d6017832 vs ba148e83316c6a24, fill d5bdf85e5b8ec998 vs73736636b17371e9, shaders ea3dbbf2ef61ae6f vs211d40e09e1c3c64. Gate correctly rejects the first mismatch. Shader scene lacks frame counters by design; API failure would fail the whole run. Pending-query887B0001 debug messages are normal WASSTILLDRAWING, not errors in the result.

The result is an image discrepancy, not a demonstrated GPU hang. Different CPU/GPU transcendental and filtering arithmetic may affect fill/shaders; draws uses point sampling and needs close inspection. No pixel data was captured in010, so the size/location of differences is not yet known. The next fresh run uses a reviewed client that dumps the exact checksummed bytes for comparison, without changing UMD or workload.

Supervisor failed-restored41.5657781s, CPU verified, baseline/postflight/tree closure verified, cleanup true. CPU171 retained, no KMD/DWM restart. Selected outputs omit module paths, environment and timing. Full raw material scratch/m14/runtime010-ops. No performance claim or broader M14 acceptance.
