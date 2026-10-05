# LAB (elevated SSH): non-invasive cdb attach (-pv) to one process; loads our own PDBs from -Sym (pushed by the
# operator) with noisy symbol loading, prints the module records of the shell, engine and ICD and -Samples stacks of
# the threads in -Tids (decimal), -Gap ms apart. Lab memory/process analysis, owner consent 2026-09-27/28.
param([string]$Image = 'ROTTR', [string]$Sym = 'C:\BC250\tmp\sym-rottr4', [string]$Tids = '', [int]$Frames = 40,
      [int]$Samples = 1, [string]$ImagePath = 'C:\BC250\m14\app-route-icdmt\gpu')
# -pv reads no image headers from the process; -i names where the DLL files are, so the PDB records can be matched.
$p = Get-Process -Name $Image -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no $Image process"; exit 2 }
$cdb = 'C:\BC250\tools\cdb\cdb.exe'
$cmd = '.symopt- 0x40; !sym noisy; .reload /f amdgpu_wddm_dxvk.dll; .reload /f amdgpu_wddm_radv.dll; !sym quiet; ' +
       'lmvm amdgpu_wddm_dxvk; lmvm amdgpu_wddm_radv; '
$threads = @($Tids -split ',' | Where-Object { $_ } | ForEach-Object { '{0:x}' -f [int]$_ })
if (-not $threads) { $threads = @() }
foreach ($t in $threads) { $cmd += ('~~[0x{0}]s; kc {1}; ' -f $t, $Frames) }
if (-not $threads) { $cmd += ('~* kc {0}; ' -f $Frames) }
$cmd += 'qd'
for ($i = 0; $i -lt $Samples; $i++) {
    "== sample $i"
    & $cdb -pv -p $p.Id -y $Sym -i $ImagePath -c $cmd 2>&1 |
        Where-Object { $_ -notmatch 'WARNING: Unable to verify|^Executable search path|Debugger Extensions Gallery|Repository :|^\s*$' } |
        Select-Object -Last 400
}
