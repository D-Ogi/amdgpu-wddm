# Errors001: native Map removal differs from the API-return oracle

Client f5e11289, shell d18fe04f, engineDC65/ICDC388 admitted by M751.
Exact files are pinned by stage-manifest.json. CPU control runs first, then GPU
injection on three separate native D3D11 devices. The client checks system d3d11.dll
and exact sibling module paths. No test ran on the development GPU.

CPU cases pass. GPU CreateBuffer returns0x8007000E with no buffer, leaves the device
healthy and permits successful creation after injection is disabled. UpdateSubresource
reports removal0x887A0005, retained after injection is disabled and Flush.
Map WRITE_DISCARD reports S_OK and a non-null API pointer, but GetDeviceRemovedReason
immediately returns0x887A0005 and remains there. The client fails this case; the whole
trial is failed-restored, not accepted by weakening the oracle.

Selected debugger lines confirm the engine injected the buffer-allocation failure and
Map called SetErrorCb with D3DDDIERR_DEVICEREMOVED0x88760870. Source clears the DDI
output before mapping and returns without publishing a pointer on that failure.
The reason the native API supplies success/a pointer remains unresolved. A runtime
fallback mapping is a hypothesis, not established by these logs. The probe never writes
through that pointer. Native API return semantics need source/docs or offline-runtime
analysis before changing the expectation. Create and Update do not depend on resolving it.

Supervisor52.3227245s; CPU171 restoration, postflight and child closure verified.
Task Missing at15:49:46Z. No DWM/OS restart; lab free. This is user-mode OOM injection,
not physical memory exhaustion or GPU device reset. Full raw records remain local at
scratch/m14/errors001-ops; only selected DDI messages exported, without addresses.
