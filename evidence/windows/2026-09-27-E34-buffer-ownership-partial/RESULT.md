# M579: buffer ownership fix, partial validation

Candidate042 UMD8CDF2C85 with hosted ICD3508416F runs the unchanged
EXE97110446 from M578. Ordinary frontend binding updates now use resource
references; final D3D buffer destruction invokes the release callback once.
The patch and exact source hashes are in the E34 hosted-runtime directory.

Control098 passes the preceding eight graphics checks, four viewport images,
and all four streams-layout-stride images. The latter no longer raises the
M578 access violation. The next vb-nooverwrite case fails its first snapshot:
4032/4096 pixels differ, first mismatch at8,0 has RGBA32,0,160,255 instead of
32,0,64,255. Exit2 is a pixel-oracle failure, not a process exception.
Later cases and the Flush-after-every-draw pattern are not reached.
M578 WARP094 and CPU095 remain the positive controls for this unchanged EXE.

The blue value matches the final pass, suggesting buffer-storage reuse or
ordering as a lead. The exact cause and connection to BD-043 remain unproven.
This is not a passing state matrix, a DWM fix, or a candidate promotion.
Sharing and Present regressions are deferred until the pixel failure is fixed.

The runner verifies restoration of baseline ICD93B1D1FD and CPU UMD8279AC7F.
CPU DWM4400 remains unchanged. Raw diagnostics and KMD logs stay private,
bound by private-hashes.json. Source parent8bf9d52; unit A,2026-09-27.
