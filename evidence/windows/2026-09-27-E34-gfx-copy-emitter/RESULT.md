# M605: GFX10 copy packet construction

The GFX10 ME DMA_DATA emitter passes host tests for chunking,64-bit address
preservation, first RAW_WAIT and final CP_SYNC, exact total byte coverage and
untouched capacity canaries. Tests include11 sizes from1 through UINT_MAX,
overlap in both directions, identity ranges, address wraparound, empty requests,
null output and adjacent valid ranges. Inadequate capacity and invalid requests
write no partial packet. Host and kernel-flag compilation pass with /W4 /WX.
All10 quick gates pass, including the new mandatory gfx-copy gate.

The builder follows the RADV GFX9+ path using selected unchanged Mesa definitions
and imported AMD nvd.h. Source hashes are attached; parent9455d0d. No GPU command
was submitted and no kernel artifact deployed in this experiment. GFX copy
execution/coherence, row integration, hardware fence retirement, residency and
CDD/DWM interop remain unverified. KMD153 and CPU desktop are unchanged by this
work; the lab is in the separate coordinated test window. Full G0 remains open.
