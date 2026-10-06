# LAB (elevated SSH): non-invasive cdb attach (-pv: threads suspended only while the debugger reads them, nothing
# injected, the process continues on detach) to one process image; prints every thread's stack as module+offset,
# with CPU times per thread first. Lab memory/process analysis, owner consent 2026-09-27/28.
# -Sym: a folder with our own PDBs (pushed by the operator) symbolizes the shell, engine and ICD frames.
param([string]$Image = 'ROTTR', [int]$Frames = 24, [string]$Kind = 'kc', [string]$Sym = 'srv*C:\BC250\tmp\sym-none')
$p = Get-Process -Name $Image -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no $Image process"; exit 2 }
"{0} pid {1} cpu {2:N1} s threads {3}" -f $Image, $p.Id, $p.TotalProcessorTime.TotalSeconds, $p.Threads.Count
$p.Threads | Sort-Object { $_.TotalProcessorTime } -Descending | Select-Object -First 12 | ForEach-Object {
    try { "  tid {0,6} cpu {1,8:N1} s state {2} wait {3}" -f $_.Id, $_.TotalProcessorTime.TotalSeconds, $_.ThreadState, $(if ($_.ThreadState -eq 'Wait') { $_.WaitReason } else { '' }) } catch { "  tid $($_.Id) unreadable" }
}
$cdb = 'C:\BC250\tools\cdb\cdb.exe'
& $cdb -pv -p $p.Id -y $Sym -c (".symopt+ 0x40; ~* {1} {0}; qd" -f $Frames, $Kind) 2>&1 |
    Where-Object { $_ -notmatch 'WARNING: Unable to verify|^Symbol search path|^Executable search path|Debugger Extensions Gallery|Repository :|^\s*$' } |
    Select-Object -Last 900
