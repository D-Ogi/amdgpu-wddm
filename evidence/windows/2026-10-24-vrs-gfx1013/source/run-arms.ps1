# vrsprobe lab step: runs the given vrsprobe arms one after another under one RADV_BC250_VRS_PROBE mask.
# Windows PowerShell 5.1 (the lab's SSH shell). No DriverStore, registry or service change: the probe ICD is a
# private DLL that vrsprobe loads directly (--icd), so only this process sees it.
#   -Mask off      RADV_BC250_VRS_PROBE unset (the probe ICD behaves as its release-line base, lab flavour)
#   -Mask 0x03     RADV_BC250_VRS_PROBE=0x03 (see LAB-PLAN.md for the bits)
#   -Arms a,b,c    vrsprobe arm names (vrsprobe --list-arms); 'caps' prints the capabilities only
#   -Out <dir>     output directory; one <arm>.txt per arm, step.txt with the summary
# Bounds: each arm has a 20 s process deadline and a 5 s fence timeout; no arm starts after 150 s of the step,
# so the step stays under three minutes. The step stops at the first arm that ends with exit 3, 4 or 5, or
# that adds a GPU fault or timeout line to the KMD log, and reports it. The output directory goes into <Out>.zip.
param([Parameter(Mandatory)][string]$Mask, [Parameter(Mandatory)][string]$Arms,
      [string]$Out = 'C:\BC250\tmp\vrs-probe\out',
      [string]$Root = 'C:\BC250\tmp\vrs-probe')
$ErrorActionPreference = 'Continue'
$client = Join-Path $Root 'vrsprobe-x64.exe'
$icd = Join-Path $Root 'icd\amdgpu_wddm_radv.dll'
New-Item -ItemType Directory -Force $Out | Out-Null
$summary = Join-Path $Out 'step.txt'
function Say($t) { $t; Add-Content -LiteralPath $summary -Value $t }
function Sha($p) { if (Test-Path -LiteralPath $p) { (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash } else { 'ABSENT' } }
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction SilentlyContinue).InstallRoot
$cli = if ($inst) { Join-Path $inst 'tools\bc250kmd_cli.exe' } else { '' }
# GPU FAULT: ih.c. Timeouts: the ResetEngine, ResetFromTimeout and RestartFromTimeout lines and the submit
# diagnostic's inactivity line of wddm.c; the periodic "wddm summary" lines are not counted.
function Faults {
    if (-not $cli -or -not (Test-Path -LiteralPath $cli)) { return [pscustomobject]@{ F = -1; T = -1; Text = 'kmd-log-unavailable' } }
    $l = @(& $cli log 2>&1 | ForEach-Object { [string]$_ } | Where-Object { $_ -notmatch 'wddm summary' })
    $f = @($l | Where-Object { $_ -match 'GPU FAULT' }).Count
    $t = @($l | Where-Object { $_ -match 'ResetEngine node|ResetFromTimeout failed|RestartFromTimeout|observed inactivity' }).Count
    [pscustomobject]@{ F = $f; T = $t; Text = ('{0} fault lines, {1} timeout lines' -f $f, $t) }
}
$t0 = Get-Date
Say ("step mask={0} arms={1} start {2}" -f $Mask, $Arms, [DateTime]::UtcNow.ToString('o'))
Say ("client {0}  {1}" -f (Sha $client), $client)
Say ("icd    {0}  {1}" -f (Sha $icd), $icd)
$before = Faults
Say "kmd before: $($before.Text)"

Remove-Item Env:\RADV_BC250_VRS_PROBE -ErrorAction SilentlyContinue
Remove-Item Env:\RADV_BC250_VRS_NOOP -ErrorAction SilentlyContinue
Remove-Item Env:\RADV_DEBUG -ErrorAction SilentlyContinue
$env:MESA_SHADER_CACHE_DISABLE = 'true'
# In a DLL the ICD sends stderr to NUL unless AMDGPU_WDDM_LOG says otherwise (util/u_amdgpu_wddm_stdio.c); the lab
# flavour's 'stderr' keeps the process's stderr, so the BC250 VRS PROBE lines reach <arm>.txt.err.
Remove-Item Env:\AMDGPU_WDDM_GAMELOG -ErrorAction SilentlyContinue
$env:AMDGPU_WDDM_LOG = 'stderr'
if ($Mask -ne 'off') {
    if ($Mask -notmatch '^0x[0-9a-fA-F]{1,2}$') { Say "refused: mask '$Mask' is not 'off' or 0xNN"; exit 2 }
    $env:RADV_BC250_VRS_PROBE = [string][Convert]::ToInt32($Mask.Substring(2), 16)
}
Say ("env RADV_BC250_VRS_PROBE={0} AMDGPU_WDDM_LOG={1}" -f $(if ($env:RADV_BC250_VRS_PROBE) { $env:RADV_BC250_VRS_PROBE } else { '(unset)' }), $env:AMDGPU_WDDM_LOG)

$stop = $null
foreach ($arm in ($Arms -split ',')) {
    $arm = $arm.Trim()
    if (-not $arm) { continue }
    if (((Get-Date) - $t0).TotalSeconds -gt 150) { Say "arm $arm not started: the step is past 150 s"; $stop = 'time'; break }
    $file = Join-Path $Out "$arm.txt"
    if ($arm -eq 'caps') { $a = @('--caps', '--icd', $icd, '--deadline', '20') }
    else { $a = @('--arm', $arm, '--icd', $icd, '--deadline', '20', '--fence-timeout-ms', '5000', '--dump', (Join-Path $Out "$arm.bin")) }
    $s = Get-Date
    # Two plain files, not PowerShell's 2>&1: PS 5.1 wraps native stderr lines as error records.
    $p = Start-Process -FilePath $client -ArgumentList $a -PassThru -NoNewWindow `
        -RedirectStandardOutput "$file.out" -RedirectStandardError "$file.err"
    $null = $p.Handle   # PS 5.1 reports no ExitCode unless the handle is opened while the process runs
    # The client ends itself at 20 s; this is the backstop for a process stuck in the kernel.
    if (-not $p.WaitForExit(30000)) {
        Say "arm ${arm}: still running after 30 s, stopping the vrsprobe process this step started (pid $($p.Id))"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        $p.WaitForExit(10000) | Out-Null
        $code = 4
    } else { $code = $p.ExitCode }
    $secs = ((Get-Date) - $s).TotalSeconds
    $lines = @(Get-Content -LiteralPath "$file.out" -ErrorAction SilentlyContinue) + @(Get-Content -LiteralPath "$file.err" -ErrorAction SilentlyContinue)
    $lines | Set-Content -LiteralPath $file
    Say ("--- {0}: exit {1} after {2:N1} s" -f $arm, $code, $secs)
    $lines | Select-String -Pattern '^RESULT|^VERDICT|^counter|^caps|^BC250 VRS PROBE|DEVICE_LOST|FENCE_TIMEOUT|DEADLINE|vrsprobe:' |
        ForEach-Object { Say ('  ' + $_.Line) }
    $now = Faults
    if ($now.F -gt $before.F -or $now.T -gt $before.T) { Say "kmd log grew after ${arm}: $($now.Text) (before: $($before.Text))"; $stop = 'kmd'; break }
    if ($code -ge 3) { Say "stop: arm $arm ended with exit $code"; $stop = "exit$code"; break }
}
Remove-Item Env:\RADV_BC250_VRS_PROBE -ErrorAction SilentlyContinue
Remove-Item Env:\MESA_SHADER_CACHE_DISABLE -ErrorAction SilentlyContinue
Remove-Item Env:\AMDGPU_WDDM_LOG -ErrorAction SilentlyContinue
Say ("kmd after: {0}" -f (Faults).Text)
Say ("step end {0} after {1:N1} s; stop={2}" -f [DateTime]::UtcNow.ToString('o'), ((Get-Date) - $t0).TotalSeconds, $(if ($stop) { $stop } else { 'none' }))
# One archive for target.py pull: step.txt, every <arm>.txt and the raw readbacks.
$zip = "$Out.zip"
Remove-Item -LiteralPath $zip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $Out '*') -DestinationPath $zip
"archive $zip"
if ($stop) { exit 1 }
exit 0
