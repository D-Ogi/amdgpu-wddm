# M763: native D3D12 adapter contract admitted against live KMD

Source bef5fb7a; adapter016 DLL768CD003 and probe7977C238 (full hashes in artifacts.json). The probe uses an explicit DLL path and a KMT-backed QueryAdapterInfo callback, not the D3D12 runtime. It opens the BC-250 selected through DXGI, queries1496 bytes of UMDRIVERPRIVATE, calls the actual OpenAdapter12 export, independently compares its public contract identity with DXGI, then closes the UMD and KMT adapters.

All calls succeed. Identity matches; contract version3, submittable node mask1, GFX ring mask1. Caps flags7 mean APU|ALL_VRAM_VISIBLE|GOLDEN_GB_ADDR; these are bc250_umd_private flags, not the health/admission flags from the driver monitor. UNMEASURED is clear.

Supervisor17.614s, exit0, Job empty, unchanged CPU171 pre/postflight boot/DWM, binary hashes/registrations, generation, epoch and health flags. No device/context/queue creation, GPU submission, driver deployment or registry modification. This establishes real KMD contract admission only; native D3D12 runtime negotiation, queue ordering and FL12_1 remain unverified.

Prior adapter-live001 failed in its PowerShell5 harness: native diagnostic stderr became NativeCommandError under ErrorActionPreference=Stop. Its complete result was rejected; Job closed and CPU171 remained unchanged18.279s. Fresh002 uses Start-Process with direct stdout/stderr redirection and10s child limit inside20s engine/170s supervisor bounds.

Logs printed by the probe contain status, size, contract masks and an identity-match boolean, not LUID/addresses/handles. Receipt omits child PID. Raw pre/postflight logs remain scratch/m15/adapter-live001 and002.
