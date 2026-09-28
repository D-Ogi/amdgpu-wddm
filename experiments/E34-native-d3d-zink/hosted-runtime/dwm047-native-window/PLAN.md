# DWM047 - native GPU window under hosted GPU DWM

Prepared, not run. Exact166/d7948d8e/C0CE, same DWM046 capture/store audit.
Add a selectively routed native hardware flip client validated under CPU DWM
in M707. Client uses --lower (y600) to avoid the existing GDI window workload.
Both processes use separate logs; only DWM reads the existing audit marker.
The DWM claim remains exclusive to dwm.exe, never consumed by the client.

Render105s, checkpoints130s, watchdog rollback140s, acceptance180s. Owner's
maximum3-minute test limit applies. Exact-path client is killed before adapter
transition on all restore paths. No permanent GPU promotion or OS reset.

Require existing DWM gates plus exact client modules,30+ textured frames,
frozen client PID/geometry across capture, both full76800 green client checks,
and successful client exit. Attribute client and DWM GPU work independently.
M13 sharing/lifecycle and longer-duration requirements remain separate.
