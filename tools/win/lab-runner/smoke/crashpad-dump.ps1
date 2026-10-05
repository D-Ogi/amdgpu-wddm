# LAB (elevated SSH, read-only): a test game's crashpad minidumps (Crystal Dynamics titles and others that keep
# crashpad\reports under the lab user's roaming profile). Lists the reports of the last -SinceMinutes and runs cdb on
# the newest one: exception record, faulting stack with module+offset (no symbols for the game), loaded graphics
# modules. Text only; the dump stays on the lab unless pulled.
# -Short: exception, stack and only our, D3D and C runtime modules (the full module list hides the stack).
param([string]$Dir = 'C:\Users\bc250\AppData\Roaming\Crystal Dynamics\Rise of the Tomb Raider\crashpad\reports',
      [int]$SinceMinutes = 120, [int]$Frames = 40, [switch]$Short)
$since = (Get-Date).AddMinutes(-$SinceMinutes)
if (-not (Test-Path $Dir)) { "no crashpad dir $Dir"; exit 2 }
$d = @(Get-ChildItem $Dir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $since } |
       Sort-Object LastWriteTime)
"reports since {0:HH:mm}Z: {1}" -f $since.ToUniversalTime(), $d.Count
$d | ForEach-Object { "  {0:HH:mm:ss}Z {1,10} {2}" -f $_.LastWriteTime.ToUniversalTime(), $_.Length, $_.Name }
$dmp = $d | Where-Object { $_.Extension -eq '.dmp' } | Select-Object -Last 1
if (-not $dmp) { "no .dmp"; exit 0 }
"--- cdb on $($dmp.Name)"
$cdb = 'C:\BC250\tools\cdb\cdb.exe'
$cmds = ".symopt+ 0x40; .ecxr; .exr -1; kv $Frames; lmvm MSVCP140; lmvm VCRUNTIME140; lmvm VCRUNTIME140_1; lmvm amdgpu_wddm_d3d11; lm; q"
if ($Short) { $cmds = ".symopt+ 0x40; .ecxr; .exr -1; kc $Frames; lm m amdgpu*; lm m d3d1*; lm m dxgi; lm m msvcp*; lm m vcruntime*; lm m bc250*; q" }
& $cdb -z $dmp.FullName -y 'srv*C:\BC250\tmp\sym-none' -c $cmds 2>&1 | Where-Object { $_ -notmatch '^\*\*\* WARNING: Unable to verify|^Symbol search path|^Executable search path' } |
    Select-Object -Last 120
