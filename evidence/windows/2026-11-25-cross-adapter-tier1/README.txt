Cross-adapter resources, tier 1 (copy), for the HLK job "WDDM2.4 Mandatory Caps"

static-reading.txt   featurecheck.dll 26100.8328, dxgkrnl.sys 26100.9974, dxgi.dll and d3d12core.dll 26100.9818:
                     what the HLK checks, what dxgkrnl does with MemoryManagementCaps.CrossAdapterResource, and the
                     positive control of the cross-adapter probe on the development PC GPU.

Fact: M972 (docs/facts/data/display.yaml). Code: driver/kmd/cross_adapter.h. Contract rule: R53.
Open: dxgmms2.sys (VidMm placement of a cross-adapter allocation) is not read; the lab run of the probe on unit A
with this KMD is the check of the placement.
