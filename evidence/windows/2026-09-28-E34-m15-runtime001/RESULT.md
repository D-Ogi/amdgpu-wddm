# M758: system D3D12 runtime version selection

Offline exact unit-A System32 D3D12Core.dll10.0.22621.5415. Identity in identity.json. PE/PDB GUID match checked locally, GUID redacted for repository evidence. PE/DBI age1; PDB info age3 recorded separately. No debugger or live breakpoints.

FillAPIVersions RVA14054 loads its table via RIP-relative LEA at1406b: next instruction14072 plus displacement144ad2 gives158b44. It iterates52 records at stride32, reading interface DWORD+0 and build WORD+4. versions.json retains those52 records. R8 entries end at builds90,91,92; no108.

ResolveUMDAndVersion calls this function at19b7f, constructs (interface<<32)|(build<<16), masks the driver's low16 bits and compares. Static code/data evidence only; live native negotiation remains unmeasured. An application-local Agility runtime can differ.

Diagnostic adapter now targets0092 instead of0108. /W4 /WX export/lifetime/context tests pass. GetCaps and FillDDITable remain unimplemented. No deployment or functional D3D12 claim. Binaries and PDB remain outside the repository under scratch/m15/runtime001 and scratch/symbols.
