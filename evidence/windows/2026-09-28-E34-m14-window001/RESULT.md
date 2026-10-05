# M741 - Window control stopped before rendering on telemetry access denial

Window001 source43f88d8c, manifest
B3213C222D753008A8AA4A5CB072D0D11ABC556CB9E059F200DBCFEAFC9C4466.
The active-console launcher correctly uses the ordinary user's token. The Cpu
phase incorrectly tried to open the privileged Bc250Rd device for temperature
inside that session; Win32 error5 stopped it before debug-child/client startup.
No D3D11 swap chain, image or Present result was obtained. GPU was not launched.

Independent SYSTEM supervisor reports failed-restored25.7720886s, baseline
restored/postflight verified/tree closed. Task cleanup succeeds; subsequent
Inspect reports Missing. CPU171 is unchanged. No KMD/DWM/OS restart.
Raw scratch/m14/window001-ops. failure.txt is a reduced diagnostic excerpt,
not raw output; full script paths and process environment are omitted.

Correction prepared separately as window002: Cpu/Gpu controllers remain SYSTEM
for STOP/temperature checks, and launch only the D3D client wrapper in the user
session. The interactive helper and its client live inside the outer phase Job;
restoration still requires a closed outer tree. No user privilege elevation or
weakened temperature check. The correction is built and source-gated, not yet
lab-validated at this measurement.
