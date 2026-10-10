# A planned restart of unit A, from the SSH session. A driver change takes effect at the restart, and the
# lab desktop route changes at a restart as well (owner, 2026-10-03: not by killing DWM).
# The boot time goes out first, so the host can wait for a boot time that differs from it instead of polling
# blind. The host waits through target.py; it never polls port 22 every second.
param([string]$Reason = 'train validation')
'boot {0:o}' -f (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
shutdown.exe /r /t 5 /c "amdgpu-wddm: $Reason"
"restart in 5 s: $Reason"
