# M747 - Native D3D11 GPU window test passes after DISPLAYABLE handling

Window006 source96615291, manifest3E52C5C8F117EFCCC65DC54933A1B49190F768C2404C389632694F29E7F73AED.
Compared with M746, the shell accepts DISPLAYABLE runtime buffers; engine253A,
ICDC0CE, ABI80352134, config and frozen client84C328 remain unchanged. Router
paths identify the new attempt. The host loader and DDI tests pass.

CPU FL10_0 and GPU FL11_1 both create the windowed FLIP_DISCARD swap chain and
exit0 using system D3D11. Client sourcef01fa71a requires S_OK from Present;
draw/fill scenes each record three successful Presents, with zero API/query
failures and no disjoint timing result. The shader scene renders offscreen.
GPU module and exact C0CE ICD checks pass. This is an API/content control, not
an independent observation of the composed screen or proof of copy-free Present.

All six PAMs are independently pulled and compared byte for byte on the host
against runtime011: both CPU and GPU draws/fill/shaders match exactly. The GPU
references also match same-GPU per-app DXVK in M739. Images are synthetic
benchmark outputs; image-equivalence.json records exact hashes and sizes.

Independent thermal supervision has no monitor error; both interactive Jobs
close. Supervisor passes49.5425695s, restores CPU171 and verifies baseline,
registry, boot/driver generation and DWM identity. Task Missing observed at
2026-09-28T14:37:01.1840669Z. No KMD/DWM/OS restart. Raw window006-ops remains
under scratch/m14. Selected JSON omits environment, module paths and timings.
The bounded debugger run has cold caches; its times are not performance proof.
A logged887B0001 is the expected pending-query status, followed by successful
query completion, not a device-removal result.

Further work includes independently measured composition/Present transport,
resize/lifecycle coverage, D3D11 feature coverage and D3D12/FL12_1. This result
does not close those requirements or justify permanent system-wide promotion.
