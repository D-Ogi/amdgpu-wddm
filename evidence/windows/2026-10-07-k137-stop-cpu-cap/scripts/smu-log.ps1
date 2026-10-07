# LAB read-only: the KMD log ring lines about SMU messages, DPM forces, CPU and fan since the current start.
$cli = 'C:\BC250\tmp\k137\bc250kmd_cli.exe'
& $cli log 2>&1 | Select-String -Pattern 'smu|SMU|msg|force|cpu|fan|vid|VID|floor|gfxoff|GfxOff|power' |
    Select-Object -First 160 | ForEach-Object { $_.Line.Trim() }
