# M769: hosted adapter policy query before device creation

Source fde9ae35, adapter028 probe, engine source 7bfcd7f0, exact binary hashes
in binaries.json. The previous adapter-caps001 attempt stopped on a PowerShell
NativeCommandError raised for informational stderr; its Job closed and postflight
passed. This fresh attempt changes only log capture to Start-Process.

QueryAdapterCaps returns S_OK for FEATURE_LEVELS, OPTIONS and OPTIONS5. The
adapter-only hosted scope closes without device callbacks or queue bindings.
Results: maximum FL11_1 (0xb100), tiled resources tier0, binding tier3, and
raytracing tier1.1 (enum value11). These are engine policy answers on unit A,
not native UMD feature support or rendered ray tracing.

The probe Job is empty, root exit0 observed, no timeout. Supervisor passes in
19.3429537 seconds. CPU171 boot, DWM, generation/epoch and registration remain
unchanged; postflight health15 and66.9C. No device, GPU work, registry edit,
PnP refresh, DWM restart or system ICD installation was requested.

The raw selected probe logs and helper receipt are unedited. Pre/postflight
JSONs retain only health and artifact fields; full registry, task and parameter
inventories remain local. binaries.json pins the exact staged files; no binary
is copied here. Native D3D12CreateDevice, resource submission, FL12_1 and game RT
remain unproven. Next: wire the delivered GetCaps mapper into the registered UMD.
