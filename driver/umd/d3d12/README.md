# D3D12 diagnostic adapter

PROVENANCE: Microsoft graphics-driver-samples (MIT), revision de4a2161991eda254013da6c18226f5ea06e4a9c, CosUmd12 adapter negotiation pattern; exact declarations from WDK10.0.26100 d3d12umddi.h.

Build with tools/build/build-umd-d3d12.ps1. It builds a separate amdgpu_wddm_d3d12.dll and runs the export-level adapter tests before reporting success. Existing DLLs are retained by hash. No registration or deployment occurs.

This is the first part of the T0 diagnostic shell. It owns a copy of the adapter callbacks, publishes the eight adapter functions and negotiates0108 as its diagnostic target. It does not claim a functional0108 device: CreateDevice, GetCaps and FillDDITable return E_NOTIMPL, and CalcPrivateDeviceSize returns0. Optional table count is0. Unsupported tables are left untouched. There are no engine calls or GPU submissions. The module must not be promoted as a working D3D12 driver.

The export test loads the actual DLL and checks null arguments, missing callbacks, count-only version query, insufficient capacity without writes, exact version and adjacent sentinel preservation, unsupported device/table rejection and adapter closure. /W4 /WX builds and the tests pass on the host. No lab run.

Next: implement measured caps and the version-specific device/queue/fence tables, create contexts through the D3D12 runtime callbacks using hRTCommandQueue, then run tools/win/d3d12queue under the bounded lab harness. Log stderr is pointer-free and flushed at the adapter boundary. Completion of T0 requires actual runtime queue/fence ordering observations; these host tests do not establish that contract.
