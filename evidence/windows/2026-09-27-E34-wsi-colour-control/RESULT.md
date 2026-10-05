# M674: direct-ICD known-colour WSI positive control

Unit A, 2026-09-27. Both interactive scheduled probes terminated with exit 0,
original processes gone and tasks removed. No DWM restart, PnP transition,
registered ICD replacement or configuration change was performed.

The probe loads ICD0CD4A98D by absolute path and dispatches through its GIPA/GDPA.
Captured module hashes confirm the chosen candidate. BC250_WSI_CPU_PRESENT=1
selects the diagnostic GDI WSI path; present.csv confirms gdi, all 60 results 0
in each run. Each probe clears the swapchain on the GPU, presents red/green/blue
phases, and holds blue for capture. Every one of the 60 render fences completed.
These fences alone do not witness a later KMT/BGP1 copy.

## Image controls

- colour001, source a25377e, EXE6722603A: full-client comparison FAIL, 36 of
  307200 pixels differ in both captures. All differences lie in the bottom
  seven rows at the two corners. The remaining 307164 pixels are exactly blue.
- colour002, source hashes in source-hashes.json, EXE228EBEC0: explicitly requests
  DWMWCP_DONOTROUND (HRESULT0), negotiates ICD interface7 and enumerates FIFO.
  Full-client comparison PASS: all 307200 pixels exactly (0,0,255) in both the
  independent primary BMP and full-size GDI PNG. Client origin328,271, extent640x480.
  The RGB client hashes match. No masking, border exclusion or tolerance is used.
- Both controls were built with /W4 /WX. The same analyzer rejects colour001
  (exit1) and accepts colour002 (exit0). Disabling rounded corners removes the
  localized discrepancy in this paired control; it was not silently masked.

The final blue frame is the checked content. The logged red/green phases are
not independently pixel-validated. Complete desktop visual correctness is not
inferred from the small client rectangle. Images stay in private scratch; their
hashes and exact pixel summaries are retained here.

## Closure and limits

colour001 closed13:31:33Z; colour002 closed13:35:43Z. Both retained CPU DWM9852,
boot10:58:57.500Z, exact KMD164/SYS9B9B99D3, UMD8279AC7F and registered ICDCF3948D6.
Health15,1000MHz/VID116; see before/after receipts for temperatures.
No permanent GPU promotion. Local scripts/archive are under
scratch/g0-hosted/wsi-colour001 and wsi-colour002.

This verifies direct-ICD Win32 WSI dispatch and a full-client image oracle on the
CPU presentation path. The real KMT Present route under GPU DWM, BGP1 attribution,
GPU copy completion and G0's steady-state no-CPU-frame-copy proof remain open.
