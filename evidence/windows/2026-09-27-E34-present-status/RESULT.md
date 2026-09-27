# GPU Present refusal classification

Source parent d98dd68 plus this change; 2026-09-27. Not deployed.

The producer now distinguishes malformed pointers/alignment from exhausted
remaining DMA/private capacity. Only the latter requests buffer rotation.
Compile-time assertions bind the fresh non-UMD context capacities to the
minimum64-byte IB and24-byte record. Failed allocation identity validation
returns STATUS_INVALID_HANDLE; unsupported color format returns
STATUS_GRAPHICS_CANNOTCOLORCONVERT. Other malformed-input paths retain their
existing failure status; this is not a complete status-policy redesign.

Reference: local WDK10.0.26100 ref/ddi-display/d3dkmddi.md,
DXGKARG_PRESENT DmaBufferPrivateDataSize (line33654) explicitly means remaining
bytes; DxgkDdiPresent return table (line9633 onward) documents exhaustion,
invalid handle and unsupported conversion.
Public reference:
https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgkarg_present

All13 quick gates pass, including actual WDDM compilation. Existing host tests
exercise identity rejection and command/record validation, but do not invoke
this DDI status adapter. Runtime retry behavior remains unverified.
No CPU fallback or success-without-copy was added. G0 remains open.
