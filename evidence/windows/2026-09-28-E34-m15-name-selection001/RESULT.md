# M766: offline DX12 name selection in the retained dxgkrnl image

Scope: the retained unit-A 22631 dxgkrnl.sys, SHA-256 AAF502AD1D6050C855480AE4057222A6375587DE0D77AEFBADF9039EF4DD72F9.
The parser verifies the PE CodeView GUID against the PDB info stream and the PE
age against DBI age (both 1). PDB info stream age is 2; this difference is
recorded rather than treated as a different binary. No live debugger, memory
reads, adapter restart or deployment was involved. This is static evidence,
not a trace of the branch taken by M765.

## Query path

SDK 10.0.26100 d3dkmthk.h defines KMTQAITYPE_UMDRIVERNAME as 1.
The image's switch table at RVA 0xA50E0 has entry 1 pointing to 0x191B92.
That branch requires size 0x20C, loads adapter+0xB70 and calls
ADAPTER_RENDER::CopyUmdFileName at 0x1942A4.

CopyUmdFileName rejects version >=6, calls GetUMDFileName, and rejects an empty
name or the two-character string <> with 0xC000000D. Otherwise it copies the
selected string. GetUMDFileName uses the current DXGPROCESS flags and the stored
ADAPTER_RENDER strings. The ordinary branch reads offset 0x140 + version*0x10;
version 3 therefore selects 0x170. Another process-flag branch selects
0x1A0 + version*0x10. Versions 4/5 select 0x230/0x240. The cold flags &0x30
branch accepts version 3 using offset 0x220 and rejects other versions.
The meanings of these private process flags have not been established here.

Neither this selector nor the copy helper rereads registry values or checks
GPU MMU/preemption capabilities. This does not rule out capability gates earlier
in adapter initialization or elsewhere in the D3D12 runtime.

## Initialization

ADAPTER_RENDER::Initialize calls InitializeUserModeDriverNames with the
source string descriptor at the parent object's +0x628, then +0x638, and output
arrays +0x140 and +0x1A0. The helper splits up to six consecutive strings into
16-byte descriptors. Candidate direct call sites were checked against the
initialization listing; the E8 scan alone is not proof that no other caller exists.

This supports a stored per-adapter name table as an explanation of M765's
unchanged effective name after a registry-only edit. It does not show every
writer or refresh mechanism, the original registry-read timing, or the actual
private process flags in the lab probe. A restart has not been tested and is
not established as sufficient. Next trace the producer of the parent's +0x628
string and adapter initialization lifetime before changing the lab lifecycle.

Raw listings remain scratch/m15/name-selection001. The repository contains the
reproduction script, identity record and selected instruction excerpts. Symbol
names are public PDB names; no private structure type names were invented.
