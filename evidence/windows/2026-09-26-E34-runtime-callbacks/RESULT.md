# M534: hardware D3D runtime callbacks

Run002 loads the experimental UMD through the native hardware adapter, creates
its virtual presentation context through the runtime callback and creates three
64x64 textures. Application MiscFlags 0, 2 and 0x900 reach CreateResource as
DDI MiscFlags 0, 2 and 2 respectively (BindFlags 0x28). AllocateCb returns success
and a nonzero allocation handle for each; each Deallocate2Cb succeeds. Exit0.

The returned hKMResource is zero for all three. ShareObjects consequently receives
zero and returns c000000d; this does not independently prove NT-security rejection
or that allocation flags alone explain the result. No rendering into runtime
allocations or native presentation is exercised by this inventory mode.

Run001 changed UserModeDriverName but the test UMD was not loaded. The positive
control is run002: a temporary router at the existing UMD path selects the probe
only when BC250_D3D_RUNTIME_PROBE is set in that process. Other processes load the
preserved CPU library. The router uses exported OpenAdapter10/10_2 entry points.
After the bounded process, original UMD bytes and the baseline ICD are restored
and hash checked. DWM PID1856 is unchanged. There is no device or OS restart.

UMD: B0A8085CC473D85851836832BFE43DE105AA4303445FD9D63FE819EA944542CD.
Control: 45574285028B6FE28F194C2D47D87B6BD9D226F32255065DEB3201DFAE8E6135.
ICD during probe: M532 7A9970CA; restored baseline: 9C40083C.
The router and the probe UMD are diagnostics, not deployable desktop drivers.
The patch is incremental after M533; raw logs preserve both attempts.
