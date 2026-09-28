# Native GPU window composition control

Prepared, not run. Intended to extend G0 evidence from GDI input windows to a
native D3D11 hardware flip client while hosted GPU DWM composes the desktop.
The source reuses the validated runtime-audit-control texture draw. It creates
only two1x1 initial input textures, draws on the GPU and performs no frame
readback or recurring CPU image upload. Static source scope is not a measured
whole-stack no-copy claim.

Build with build-gpu-window-control.ps1 (/W4 /WX). Do not launch on the host.
The process needs an existing fresh private directory and an interactive lab
session. It moves a320x240 topmost window, alternates sampled colours, then
reacts to freeze by rendering green and waiting for DwmFlush. Atomic heartbeat
records the full screen-space client rectangle, process ID, QPC and frame count.
Stop is accepted as success only after animation and freeze. A120-second loop
deadline does not bound a hung D3D call: an independent runner watchdog remains
mandatory. The help command is the only host execution during build.

Before a lab run, integrate selective routing for this exact executable path,
separate logs/marker files from DWM, exact UMD/ICD module witnesses, fresh STOP/
thermal checks and restoration. The existing DWM046 router selects only dwm.exe
and is insufficient. Do not interpret an unmodified-router run as GPU rendering.
A CPU-DWM client control precedes the combined GPU-DWM run.

Capture only after a frozen receipt from the live expected PID. Preserve capture
start/end QPC, unchanged rectangle/frame count across capture and both primary
and composed images. check-gpu-window.py requires76800 exact green pixels;
its synthetic controls reject a single wrong pixel and invalid freeze/geometry.
Validate Present/import/fence stream, context-specific ETW GPU work and DWM
output separately. No cross-process sharing/lifecycle acceptance is inferred
from this one window; explicit M13.2 controls remain required.
