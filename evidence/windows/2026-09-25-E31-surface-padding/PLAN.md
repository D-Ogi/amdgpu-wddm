# Surface padding regression (M10 prerequisite)

Hypothesis: Notepad's imported 1428x33 RGBA surface is backed by 188496 bytes
rounded to 192512 bytes, while llvmpipe accesses 4x4 stamps and declares a
205632-byte image stride (5712*36). The failing load reaches unmapped row34.

Evidence: private full Notepad dump, captured once with explicit owner consent,
full-layout.txt and full-surface.txt in ../notepad-crash. Two crash captures agree
on shade_quads -> lp_rast_linear_rect_fallback and the failed third row load.

Change: owned surface pitch aligned to64 bytes (256 for primary), height to4;
retain imported pitch and extent, rotate metadata with storage, and validate
external storage against the raster block contract. KMD147 and Vulkan ICD unchanged.

Acceptance: five shared surface extents (64x32,1428x33,1366x35,1x1,67x65), all pixels
red A-to-B and blue B-to-A after event completion, 640x480 green readback and
presentation; correct DLL witness; Notepad launch remains responsive instead of
crashing within4seconds. No formal M10 acceptance from these D3D tests.
