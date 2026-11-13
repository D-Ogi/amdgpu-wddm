# umdlog-probe

A console probe for the UMD logging of renamed buffers (BD-379). The probe has no window. It records the events of
the WDDM 1.2 UMD logging provider (Microsoft-Windows-D3DUmdLogging) that the installed D3D11 driver writes. It prints
the events in their order and checks them against the model of the HLK test UMDLoggingRenamedSurface.

The probe does the steps of one buffer case of that test:

1. It creates two DYNAMIC buffers A and B (512 KiB by default) and one DEFAULT buffer on the BC-250 adapter (vendor
   1002, device 13FE).
2. It starts a real-time ETW session, enables the provider and requests a capture state (the rundown).
3. It does N rounds (4 by default). In each round it copies A to the DEFAULT buffer, then maps A with
   `D3D11_MAP_WRITE_DISCARD` and unmaps it. Then it does the same for B. The copy keeps the GPU on the current
   storage, so each discard renames the buffer.
4. It submits the work, waits 1 s, stops the session and prints each event of its own process.

Each output line has the time in ms, the event (`MAP`, `UNMAP` or `RUNDOWN`), the buffer (`A`, `B` or `?`), the two
handles, the offset, the size, the usage bits (`Packed` = 1, `Renamed` = 2) and the semantic. Lines that start with
`--` are the steps of the probe. The probe finds the buffer of an event from the step in which the event comes,
because the engine writes the events inside the Map call.

For each buffer, these checks must pass:

| Check | Model of the test |
|---|---|
| MAP before UNMAP | The MAP of the new instance comes before the UNMAP of the old one. |
| Renamed | Each event of the buffer has Renamed usage. |
| Own allocation | Each instance is a kernel allocation of its own: not Packed, offset 0. |
| Link | Each MAP comes while an earlier instance of the buffer is mapped. |
| Instances | At the end, 2 to N+2 instances are mapped. |

Two more checks apply to the run: no allocation is an instance of both buffers, and the session lost no event.

```
umdlog-probe [--rounds N] [--size BYTES] [--any-adapter]
```

Exit codes: 0 pass, 1 fail, 2 usage, 3 setup (no adapter, no device, no ETW session). Start the probe as an
administrator or as a member of Performance Log Users, because it starts a trace session. `--any-adapter` takes the
first hardware adapter, for a control on another driver.

Controls:

- `--size 65536` gives a buffer below `d3d11.renameDedicatedSize`. The engine suballocates it, so the "Own allocation"
  check must fail. This shows that the probe can see the packed form.
- D3D11 files from before BD-379 write UNMAP before MAP, so the "MAP before UNMAP" check must fail with them.

The probe only shows what the driver writes. The HLK test also reads the events of the kernel (DxgKrnl), and the
probe does not record those.

Build: `pwsh -NoProfile -File tools\win\umdlog-probe\build.ps1 [-Out <dir>]` (clang-cl of LLVM 23.1.4, x86-64-v3).
