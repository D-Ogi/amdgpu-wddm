# LAB: planned restart after a package install (driver change takes effect at the restart). Prints the boot time first.
'boot {0:o}' -f (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
shutdown.exe /r /t 5 /c "amdgpu-wddm package install: restart"
'restart in 5 s'
