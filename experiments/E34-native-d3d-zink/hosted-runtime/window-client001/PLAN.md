# Window client001 - visible native GPU window under CPU DWM

Prepared, not run. Exact166/d7948d8e/C0CE, CPU DWM retained. The router selects
only this exact staged gpu-window-control.exe, with fresh enable and probe env.
Same tested restoration/watchdog design as audit-client007.45s worker deadline,
90s independent watchdog. No registry, KMD or DWM replacement.

Require exact live module hashes,30 animated GPU draw/Present frames before
freeze, receipt PID matching the live process, frozen geometry unchanged across
primary/composed capture, successful process termination and unchanged DWM/boot.
Host pixel checker must confirm all76800 green client pixels in both images.
A readback from the application is not a substitute. Preserve failed captures.
No no-copy/G0 or cross-process lifecycle acceptance inferred from this control.
STOP and thermal gates apply; baseline8279/CF39 restored and tasks removed.
