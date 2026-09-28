# M634: standard-GDI allocation policy before an interop trial

The captured first Present callbacks after159 startup report flags0xC and
zero DMA size/VA. These bounded records do not exercise the Blt producer.
A quiet desktop with only the GPU Blt gate enabled would not prove BGP1.
The interop capability remains off pending its actual contract and test.

Source correction: standard GDI texture1, staging3 and lookup4 are no longer
marked CpuVisible or AccessedPhysically. CPU staging2 stays CPU-visible,
cached and restricted to coherent aperture. Legacy type0 retains all four
shared/cached resource-policy combinations. Standard surface policy overrides
irrelevant legacy hints. The helper drives CreateAllocation's actual flags.
GetStandardAllocationDriverData, CreateAllocation and OpenAllocation reject
unsupported type5 existing-system-memory ownership and types6..8 (reserved or
cross-adapter); layout arithmetic alone does not admit an allocation. No GDI
hardware acceleration or interop capability is newly advertised.

Normative source: local WDK26100 d3dkmdt.md GDISURFACETYPE, declarations from
10.0.26100, especially the CPU visibility and coherent aperture requirements.
Public reference: [GDISURFACETYPE](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmdt/ne-d3dkmdt-_d3dkmdt_gdisurfacetype).
[Residency overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/residency-overview)
was checked2026-09-27: Present allocation lists carry parameters, not residency.
The CDD-owned device contract and observed requested types still need review
before an interop trial. BC2C UMD admission remains withheld.

All13 gates and actual WDDM compilation pass. The surface gate executes58260
checks including28 new allocation-policy assertions from independent contract
fixtures, unchanged legacy combinations and rejection without output mutation.
These are host tests, not runtime confirmation of standard-GDI objects.
No lab change:159/5C640976 and CPU desktop remain. Full G0 remains open.
Logs are copied unchanged; Present excerpts retain original lines with source
filenames. No selected private values were redacted.
