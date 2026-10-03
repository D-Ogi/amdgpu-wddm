# Host test of the BD-060 rules in common.ps1 and dwm-session.ps1: the INF Reboot directive (the GPU changes driver at
# the next restart, not under the running desktop), the pnputil outcomes (only what the exit code establishes; the
# BD-059 session marker stays the KMD's), the install inputs that the argument-free run after a restart takes from the
# state, and the DWM baseline: a replacement is reported only when observed against a whole record of the same boot,
# session and logon, otherwise the history is unknown. Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-session-checks.ps1 [-Installer <package>\installer] [-Inf <packaged INF>] [-WorkRoot <dir>]
# -Inf: the INF of a built package, which must carry the directive. The baseline cases write only files under
# -WorkRoot. Nothing in the registry is written; this computer's own session and DWM are read only.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$Inf, [string]$WorkRoot = (Join-Path (Split-Path (Split-Path $Installer)) 'test-tmp'))
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
. (Join-Path $Installer 'dwm-session.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Split-Lines([string]$Text) { return , ($Text -split "`r?`n") }

# The shape of driver\kmd\bc250kmd.inf: [Manufacturer] -> Models.NTamd64 -> Bc250_Install, commented-out lines that
# the lab build switches on, and a service section that is not an install section. Its values do not track the KMD:
# the INF of a built package is checked through -Inf.
$lab = @'
[Version]
Signature   = "$Windows NT$"
DriverVer   = 10/03/2026,0.7.198.2

[Manufacturer]
%Provider% = Models,NTamd64

[Models.NTamd64]
%DeviceName% = Bc250_Install, PCI\VEN_1002&DEV_13FE

[Bc250_Install]
FeatureScore = F8
CopyFiles    = Bc250_Files                      ;@PLAIN-ONLY
;@UMD AddReg       = Bc250_UserModeDriver

[Bc250_Files]
bc250kmd.sys

[Bc250_Install.Services]
AddService = bc250kmd, 0x00000002, Bc250_Service

[Strings]
Provider   = "amdgpu-wddm"
DeviceName = "BC-250 GPU (amdgpu-wddm)"
'@
$lab = $lab -replace "`r?`n", "`r`n"

'INF install sections and the Reboot directive'
$sections = Get-InfInstallSections (Split-Lines $lab)
Check (($sections.Count -eq 1) -and ($sections[0] -eq 'Bc250_Install')) "install sections through [Manufacturer]: $($sections -join ', ')"
Check (-not (Test-InfDefersDeviceRestart (Split-Lines $lab))) 'the lab INF restarts a started device in place (no Reboot)'
$rel = Add-InfRebootDirective $lab
Check (Test-InfDefersDeviceRestart (Split-Lines $rel)) 'the release rewrite adds Reboot to [Bc250_Install]'
$a = Split-Lines $lab; $b = Split-Lines $rel
$i = [array]::IndexOf($a, '[Bc250_Install]')
$rest = @($b | Select-Object -First $i -Skip 0) + @($b | Select-Object -Skip ($i + 2))
Check (($b.Count -eq $a.Count + 1) -and ($b[$i + 1] -match '^Reboot\s+;') -and (($rest -join "`n") -eq ((@($a | Select-Object -First $i) + @($a | Select-Object -Skip ($i + 1))) -join "`n"))) 'exactly one line added, right after the section header; every other line unchanged'
Check (($rel -split "`r`n").Count -eq ($rel -split "`n").Count) 'CRLF kept on every line'
Check ((Add-InfRebootDirective $rel) -ceq $rel) 'the rewrite is idempotent'
$lf = $lab -replace "`r`n", "`n"
$relLf = Add-InfRebootDirective $lf
Check (($relLf -notmatch "`r") -and (Test-InfDefersDeviceRestart (Split-Lines $relLf))) 'an LF INF stays LF'
Check (-not (Test-InfDefersDeviceRestart (Split-Lines ($lab -replace 'FeatureScore = F8', "FeatureScore = F8`r`n; Reboot")))) 'a commented-out Reboot does not count'
Check (-not (Test-InfDefersDeviceRestart (Split-Lines ($lab -replace '\[Bc250_Files\]\r\n', "[Bc250_Files]`r`nReboot`r`n")))) 'Reboot in another section does not count'
$two = $lab -replace '(%DeviceName% = Bc250_Install, [^\r]+)', ("`$1`r`n%Other% = Other_Install, PCI\VEN_1002&DEV_FFFF") -replace '\[Bc250_Files\]', "[Other_Install]`r`nCopyFiles = Bc250_Files`r`n`r`n[Bc250_Files]"
Check ((Get-InfInstallSections (Split-Lines $two)).Count -eq 2) 'two models lines: two install sections'
Check (-not (Test-InfDefersDeviceRestart (Split-Lines ($two -replace '\[Bc250_Install\]\r\n', "[Bc250_Install]`r`nReboot`r`n")))) 'Reboot in only one of two install sections does not count'
Check (Test-InfDefersDeviceRestart (Split-Lines (Add-InfRebootDirective $two))) 'the rewrite covers every install section'
$threw = $false; try { [void](Add-InfRebootDirective "[Version]`r`nSignature = x`r`n") } catch { $threw = $true }
Check $threw 'an INF without install sections is refused'
if ($Inf) {
    Check (Test-InfDefersDeviceRestart ([IO.File]::ReadAllLines($Inf))) "the packaged INF carries Reboot in every install section ($Inf)"
}

'pnputil outcomes: only what the exit code establishes'
$o = Get-DriverPackageOutcome 3010
Check ($o.ok -and $o.deferred -and ($o.text -eq 'pnputil exit 3010: completed, a system restart is required')) "3010: $($o.text)"
$o = Get-DriverPackageOutcome 0
Check ($o.ok -and -not $o.deferred -and ($o.text -eq 'pnputil exit 0: completed')) "0: $($o.text)"
$o = Get-DriverPackageOutcome 259
Check ($o.ok -and -not $o.deferred -and ($o.text -eq 'pnputil exit 259: no device change reported')) "259: $($o.text)"
Check (-not (Get-DriverPackageOutcome 5).ok) 'any other exit code fails'
Check (@(3010, 0, 259 | ForEach-Object { (Get-DriverPackageOutcome $_).PSObject.Properties.Name } | Where-Object { $_ -notin @('ok', 'deferred', 'text') }).Count -eq 0) 'no outcome claims what the device did (no in-place field)'
Check (-not (Get-Command Test-StaleInteropMarker -ErrorAction SilentlyContinue)) 'the BD-059 session marker stays under KMD ownership: no installer function judges or removes it'

'install inputs across the argument-free continuation (RunOnce runs install.cmd without arguments)'
# The state as the next run reads it: saved as JSON, read back.
function Step-State($State, [string]$Phase) { Set-StateValue $State 'phase' $Phase; return ($State | ConvertTo-Json -Depth 6 | ConvertFrom-Json) }
function Format-Inputs($In) { return "fw=$($In.firmware_dir) params=$((@($In.parameters.Keys | Sort-Object) | ForEach-Object { "$_=$($In.parameters[$_])" }) -join ',') switches=$(@($In.switches) -join ',')" }
$none = @{}
$v11 = '0.7.199.100-tester.11'
foreach ($resumePhase in 'driver-pending-restart', 'testsigning-pending') {
    # Offline fresh install: phase 1 saves the inputs, the run after the restart has no arguments.
    $st = [pscustomobject]@{ schema = 1; phase = 'new'; package_version = $v11 }
    $first = Get-InstallInputs @{ FirmwareDir = 'D:\fw offline'; DpmMaxMHz = 1200; CuMode = 0; NoControlApp = [switch]$true; NoReboot = [switch]$true } $st $v11
    Save-InstallInputs $st $first
    $next = Get-InstallInputs $none (Step-State $st $resumePhase) $v11
    Check ((Format-Inputs $next) -eq 'fw=D:\fw offline params=DpmMaxMHz=1200 switches=NoControlApp,NoReboot') "fresh offline install resumed at ${resumePhase}: $(Format-Inputs $next)"
    Check (@($next.restored).Count -eq 4) "fresh install at ${resumePhase}: every restored input is named ($($next.restored -join ' '))"
}
# Upgrade over a verified install: the upgrade's own arguments are saved and come back after the driver package restart.
$st = [pscustomobject]@{ schema = 1; phase = 'verified'; package_version = '0.7.198.100-tester.10' }
$up = Get-InstallInputs @{ FirmwareDir = 'E:\fw'; CuMode = 40; Force = [switch]$true } $st $v11
Check (@($up.restored).Count -eq 0) 'upgrade from a verified install: nothing from the earlier install is taken'
Save-InstallInputs $st $up
Set-StateValue $st 'package_version' $v11
$next = Get-InstallInputs $none (Step-State $st 'driver-pending-restart') $v11
Check ((Format-Inputs $next) -eq 'fw=E:\fw params=CuMode=40 switches=Force') "upgrade resumed without arguments: $(Format-Inputs $next)"
$st2 = Step-State $st 'driver-pending-restart'
$next = Get-InstallInputs @{ DpmMaxMHz = 1100; NoReboot = [switch]$true; Force = [switch]$false } $st2 $v11
Check ((Format-Inputs $next) -eq 'fw=E:\fw params=DpmMaxMHz=1100 switches=NoReboot') "a resumed run's own arguments win (settings as a set, -Force:`$false): $(Format-Inputs $next)"
$next = Get-InstallInputs $none $st2 '0.7.199.100-tester.12'
Check ((Format-Inputs $next) -eq 'fw= params= switches=') 'another package version run over the waiting state takes nothing from it'
$next = Get-InstallInputs $none (Step-State $st 'install-incomplete') $v11
Check ((Format-Inputs $next) -eq 'fw= params= switches=') 'a re-run after a failed step is not an argument-free continuation: nothing restored'
Clear-InstallInputs $st
$next = Get-InstallInputs $none (Step-State $st 'driver-pending-restart') $v11
Check ((Format-Inputs $next) -eq 'fw= params= switches=') 'cleared at completion: nothing restored'
# The state of a tester.10 phase 1 (no install_switches) still resumes its folder and settings.
$old = [pscustomobject]@{ schema = 1; phase = 'testsigning-pending'; package_version = '0.7.198.100-tester.10'; firmware_source_dir = 'C:\amdgpu-wddm-firmware'; command_line_parameters = [pscustomobject]@{ DpmMaxMHz = 1500 } } | ConvertTo-Json | ConvertFrom-Json
Check ((Format-Inputs (Get-InstallInputs $none $old '0.7.198.100-tester.10')) -eq 'fw=C:\amdgpu-wddm-firmware params=DpmMaxMHz=1500 switches=') 'a tester.10 state at testsigning-pending resumes its folder and settings'

$ia = Get-InstallAction -State ([pscustomobject]@{ phase = 'driver-pending-restart' }) -PackageVersion $v11 -InstalledVersion $v11 -Repair $false
Check (($ia.action -eq 'resume') -and ($ia.message -match 'continuing the installation')) "same version at driver-pending-restart: $($ia.action) (not a repair, previous_package_version kept)"
$ia = Get-InstallAction -State ([pscustomobject]@{ phase = 'driver-pending-restart' }) -PackageVersion '0.7.199.100-tester.12' -InstalledVersion $v11 -Repair $false
Check ($ia.action -eq 'upgrade') "another version over a waiting install: $($ia.action)"

'DWM baseline: a replacement only when observed (dwm-session.ps1)'
$work = Join-Path $WorkRoot ('session-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
[void][IO.Directory]::CreateDirectory($work)
try {
    $file = Join-Path $work 'dwm-baseline.json'
    $u = { param([string]$t) [datetime]::SpecifyKind([datetime]$t, 'Utc') }
    $e1 = [pscustomobject]@{ boot_utc = (& $u '2026-10-03T20:33:05'); session = 1; logon_utc = (& $u '2026-10-03T20:33:40') }
    $first = [pscustomobject]@{ pid = 1916; created_utc = (& $u '2026-10-03T20:33:31.1234567') }
    $second = [pscustomobject]@{ pid = 13376; created_utc = (& $u '2026-10-03T20:35:02.5') }
    Check ((Get-DwmReplacementFinding $null @($first) $e1).state -eq 'unknown') 'no record: unknown history, neither restarted nor healthy'
    $s1 = Save-DwmBaseline $e1 @($first) $file
    Check ($s1.written -and $s1.record.dwm_pid -eq 1916) 'the first DWM of an epoch is recorded'
    $s2 = Save-DwmBaseline $e1 @($second) $file
    Check ((-not $s2.written) -and ($s2.record.dwm_pid -eq 1916)) 'a later call of the same epoch keeps the first record'
    $recs = Read-DwmBaseline $file
    Check ($recs.Count -eq 1) 'one record per epoch'
    $near = [pscustomobject]@{ boot_utc = $e1.boot_utc.AddSeconds(1); session = 1; logon_utc = $e1.logon_utc }
    Check ($null -ne (Find-DwmBaseline $recs $near)) 'the same boot read 1 s apart matches'
    Check ($null -eq (Find-DwmBaseline $recs ([pscustomobject]@{ boot_utc = $e1.boot_utc; session = 1; logon_utc = $e1.logon_utc.AddMinutes(10) }))) 'a new sign-in (another logon time) does not match: the baseline resets'
    Check ($null -eq (Find-DwmBaseline $recs ([pscustomobject]@{ boot_utc = $e1.boot_utc.AddHours(1); session = 1; logon_utc = $e1.logon_utc }))) 'another boot does not match'
    Check ($null -eq (Find-DwmBaseline $recs ([pscustomobject]@{ boot_utc = $e1.boot_utc; session = 2; logon_utc = $e1.logon_utc }))) 'another session does not match'
    $rec = Find-DwmBaseline $recs $e1
    $f = Get-DwmReplacementFinding $rec @([pscustomobject]@{ pid = 1916; created_utc = $first.created_utc.AddMilliseconds(400) }) $e1
    Check (($f.state -eq 'same') -and ($f.detail -match '^same DWM instance since the record at .+: DWM 1916 \(created 2026-10-03 20:33:31Z\) \(an earlier replacement is not excluded\)$') -and ($f.detail -notmatch 'no restart|first')) "the recorded instance still runs, and an earlier replacement stays possible: $($f.detail)"
    $f = Get-DwmReplacementFinding $rec @($second) $e1
    Check (($f.state -eq 'observed') -and ($f.detail -match '^observed: DWM 1916 \(created 2026-10-03 20:33:31Z\), recorded at .+, was replaced by DWM 13376 \(created 2026-10-03 20:35:02Z\)\. WinUI pointer-input loss after the desktop compositor \(DWM\) is terminated and restarted reproduces on this Windows build also with Microsoft Basic Display; restart Windows to recover\. A DWM crash can still be a driver defect: report it with a bug report\.$')) 'another instance than the recorded one: replacement observed, remedy restart, still reportable'
    $f = Get-DwmReplacementFinding $rec @([pscustomobject]@{ pid = 1916; created_utc = $first.created_utc.AddMinutes(5) }) $e1
    Check ($f.state -eq 'observed') 'the same process ID with another creation time is another instance'
    Check ((Get-DwmReplacementFinding $rec @() $e1).state -eq 'unknown') 'no DWM in the session: unknown'
    Check ((Get-DwmReplacementFinding $rec @($first) ([pscustomobject]@{ boot_utc = $e1.boot_utc; session = 0; logon_utc = $null })).state -eq 'unknown') 'no desktop session: unknown'
    Check (-not (Save-DwmBaseline ([pscustomobject]@{ boot_utc = $e1.boot_utc; session = 1; logon_utc = $null }) @($first) (Join-Path $work 'none.json')).written) 'nobody logged on: nothing recorded'
    Check (-not (Save-DwmBaseline ([pscustomobject]@{ boot_utc = $e1.boot_utc.AddDays(1); session = 1; logon_utc = $e1.logon_utc.AddDays(1) }) @($first, $second) $file).written) 'two DWMs in the session at once: no baseline recorded'
    for ($i = 1; $i -le 20; $i++) { [void](Save-DwmBaseline ([pscustomobject]@{ boot_utc = $e1.boot_utc.AddDays($i); session = 1; logon_utc = $e1.logon_utc.AddDays($i) }) @($first) $file) }
    Check ((Read-DwmBaseline $file).Count -eq 16) 'the file keeps the last 16 records'
    [IO.File]::WriteAllText($file, '{ not json')
    Check ((Read-DwmBaseline $file).Count -eq 0) 'a damaged file reads as no record (unknown history)'
    Check ((Save-DwmBaseline $e1 @($first) $file).written -and ((Read-DwmBaseline $file).Count -eq 1)) 'a damaged file is replaced by a new one'
    # A record of this epoch with a malformed field: ignored (unknown history, never a false replacement), and the next
    # recording of the epoch replaces it. A whole record of another epoch in the same file stays.
    $good = Read-DwmBaseline $file | Select-Object -First 1
    $other = [ordered]@{ boot_utc = $e1.boot_utc.AddDays(-1).ToString('o'); session = 1; logon_utc = $e1.logon_utc.AddDays(-1).ToString('o'); dwm_pid = 777; dwm_created_utc = $e1.boot_utc.AddDays(-1).ToString('o'); recorded_utc = $e1.boot_utc.AddDays(-1).ToString('o'); recorded_by = 'start-confirm' }
    $bad = @(
        @{ name = 'dwm_pid missing'; drop = 'dwm_pid' }
        @{ name = 'dwm_pid as text'; set = @{ dwm_pid = '1916' } }
        @{ name = 'dwm_pid 0'; set = @{ dwm_pid = 0 } }
        @{ name = 'dwm_pid null'; set = @{ dwm_pid = $null } }
        @{ name = 'session as text'; set = @{ session = '1' } }
        @{ name = 'dwm_created_utc missing'; drop = 'dwm_created_utc' }
        @{ name = 'dwm_created_utc not a time'; set = @{ dwm_created_utc = 'yesterday' } }
        @{ name = 'recorded_utc missing'; drop = 'recorded_utc' }
        @{ name = 'recorded_utc a number'; set = @{ recorded_utc = 5 } }
    )
    foreach ($b in $bad) {
        $r = [ordered]@{}
        foreach ($p in $good.PSObject.Properties) { if ($p.Name -ne $b.drop) { $r[$p.Name] = $p.Value } }
        if ($b.set) { foreach ($k in $b.set.Keys) { $r[$k] = $b.set[$k] } }
        [IO.File]::WriteAllText($file, ([ordered]@{ schema = 1; records = @([pscustomobject]$other, [pscustomobject]$r) } | ConvertTo-Json -Depth 4))
        $recs = Read-DwmBaseline $file
        $found = Find-DwmBaseline $recs $e1
        $f = Get-DwmReplacementFinding $found @($second) $e1
        $f2 = Get-DwmReplacementFinding $recs[1] @($second) $e1
        $s = Save-DwmBaseline $e1 @($second) $file
        $after = Read-DwmBaseline $file
        Check (($null -eq $found) -and ($f.state -eq 'unknown') -and ($f2.state -eq 'unknown') -and $s.written -and ($after.Count -eq 2) -and ([int]$after[0].dwm_pid -eq 777) -and ([int]$after[1].dwm_pid -eq 13376) -and ($null -ne (Find-DwmBaseline $after $e1))) "$($b.name): unknown history, the next recording replaces it, the other epoch's record stays"
    }
    Check ((Compare-DwmReadings @($first) @($first)) -eq 'same DWM instance: DWM 1916 (created 2026-10-03 20:33:31Z)') 'upgrade observation: same DWM instance'
    Check ((Compare-DwmReadings @($first) @($second)) -eq 'DWM replaced: DWM 1916 (created 2026-10-03 20:33:31Z) -> DWM 13376 (created 2026-10-03 20:35:02Z)') 'upgrade observation: DWM replaced'
    Check ((Compare-DwmReadings @($first) @()) -match 'after: none$') 'upgrade observation: no DWM after'
} finally { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
Check (-not (Test-Path -LiteralPath $work)) 'scratch folder removed'

# This computer's own session, read only: the epoch and the DWM instance must be readable without elevation. With no
# baseline the finding is unknown history, whatever the DWM's age.
$ep = Get-DwmEpoch
if ($ep.session -eq 0) { "  (no desktop session: skipped)" }
else {
    $mine = Get-SessionDwm $ep.session
    Check (($null -ne $ep.logon_utc) -and ($ep.logon_utc -lt [DateTime]::UtcNow) -and ($ep.boot_utc -lt [DateTime]::UtcNow) -and ($mine.Count -ge 1)) "this computer: session $($ep.session), logon $(Format-DwmTime $ep.logon_utc), boot $(Format-DwmTime $ep.boot_utc), $(@($mine | ForEach-Object { Format-DwmInstance $_ }) -join ', ')"
    Check ((Get-DwmReplacementFinding $null $mine $ep).state -eq 'unknown') 'this computer without a record: unknown history (no false restart)'
}

if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'session checks: all checks passed'
exit 0
