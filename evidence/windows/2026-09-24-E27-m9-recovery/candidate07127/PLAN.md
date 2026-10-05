# M404 - Full WDDM without RLC-SMU GFXOFF handshake

STOP/currentstate and exact127hash preflight. Install without baselineOSreset.
Firstfull start must log pp_gfxoff0 and pass64KiB3cycleGPUoracle. Preserve
outputs, stop and verify retirement, then one warm start. If successful,
run a separate post-warm GPU oracle before reporting recovery. If unavailable,
pinnednetworkdiscovery then one recordedAC recovery; collect persisted logs.
No unchanged retry. Compare RLC/GRBM samples, keeping all required TLB accesses.
