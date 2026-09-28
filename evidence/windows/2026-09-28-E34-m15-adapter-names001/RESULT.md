# M764: no DX12 UMD name on the current three-entry baseline

Read-only KMTQAITYPE_UMDRIVERNAME on the BC-250 adapter:

- KMTUMDVERSION_DX9 (0): STATUS_SUCCESS, bc250umd.dll.
- DX10 (1) and DX11 (2): STATUS_SUCCESS, bc250d3d.dll.
- DX12 (3): STATUS_INVALID_PARAMETER (0xC000000D), no name returned.

The unchanged baseline UserModeDriverName contains three entries. The Microsoft compute-only sample cosdriver/cos.inf registers four entries. That is the next registration hypothesis to test; this run does not prove that appending an entry is sufficient or that dxgkrnl refreshes it without restarting the adapter. It does not test D3D12CreateDevice or resolve the queue/fence contract.

The same process still passes the real KMD-backed OpenAdapter12 contract and identity check and closes both adapters. Exact DLL/probe hashes in artifacts.json; source is adapter-kmt-probe.cpp committed with this evidence. Supervisor18.729s, exit0, Job empty, unchanged CPU171 boot/DWM, binaries/registration, generation/epoch/flags. No registry mutation, deployment, device creation or GPU work.

Only basenames are printed, not full registration paths or adapter identifiers. Raw pre/postflight logs remain scratch/m15/adapter-names001. Next is a bounded, reversible fourth-slot experiment with effective-name readback before attempting the system D3D12 runtime.
