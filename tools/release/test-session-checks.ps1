# Host test of BD-060 and BD-152: deferred driver-set staging (the GPU changes driver at shutdown), the legacy
# pnputil outcomes used by rollback readers (only what the exit code establishes; the
# BD-059 session marker stays the KMD's), the install inputs that the argument-free run after a restart takes from the
# state, and the DWM baseline: a replacement is reported only when observed against a whole record of the same boot,
# session and logon, otherwise the history is unknown. Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-session-checks.ps1 [-Installer <package>\installer] [-Inf <packaged INF>] [-WorkRoot <dir>]
# -Inf: the INF of a built package, which must not carry Reboot. The baseline cases write only files under
# -WorkRoot. Nothing in the registry is written; this computer's own session and DWM are read only.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$Inf, [string]$WorkRoot)
# Default scratch: beside the folder the zip unpacks to, never inside it. The package root is files\ of that folder, so one more level up than it used to be; an older flat package keeps the old place.
if (-not $PSBoundParameters.ContainsKey('WorkRoot')) {
    $packageRoot = Split-Path $Installer   # <package>\installer -> <package>
    $packageTop = $(if ((Split-Path $packageRoot -Leaf) -eq 'files') { Split-Path $packageRoot } else { $packageRoot })
    $WorkRoot = Join-Path (Split-Path $packageTop) 'test-tmp'
}
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
. (Join-Path $Installer 'deferred-driver.ps1')
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

'INF install sections and legacy record reading'
$sections = Get-InfInstallSections (Split-Lines $lab)
Check (($sections.Count -eq 1) -and ($sections[0] -eq 'Bc250_Install')) "install sections through [Manufacturer]: $($sections -join ', ')"
Check (-not (Test-InfHasReboot (Split-Lines $lab))) 'the display INF has no Reboot directive'
$rel = $lab -replace '\[Bc250_Install\]\r\n', "[Bc250_Install]`r`nReboot`r`n"
Check (Test-InfDefersDeviceRestart (Split-Lines $rel)) 'the legacy reader recognizes an old rollback package'
Check (Test-InfHasReboot (Split-Lines $rel)) 'the new reader rejects the legacy directive for new packages'
Check (-not (Test-InfDefersDeviceRestart (Split-Lines ($lab -replace 'FeatureScore = F8', "FeatureScore = F8`r`n; Reboot")))) 'a commented-out Reboot does not count'
Check (-not (Test-InfDefersDeviceRestart (Split-Lines ($lab -replace '\[Bc250_Files\]\r\n', "[Bc250_Files]`r`nReboot`r`n")))) 'Reboot in another section does not count'
$two = $lab -replace '(%DeviceName% = Bc250_Install, [^\r]+)', ("`$1`r`n%Other% = Other_Install, PCI\VEN_1002&DEV_FFFF") -replace '\[Bc250_Files\]', "[Other_Install]`r`nCopyFiles = Bc250_Files`r`n`r`n[Bc250_Files]"
Check ((Get-InfInstallSections (Split-Lines $two)).Count -eq 2) 'two models lines: two install sections'
Check (-not (Test-InfDefersDeviceRestart (Split-Lines ($two -replace '\[Bc250_Install\]\r\n', "[Bc250_Install]`r`nReboot`r`n")))) 'Reboot in only one of two install sections does not count'
if ($Inf) {
    Check (-not (Test-InfHasReboot ([IO.File]::ReadAllLines($Inf)))) "the packaged display INF has no Reboot ($Inf)"
}

'INSTALL_AS_SET transport and restart receipt (no native driver operation)'
$stageDir = Join-Path $WorkRoot ('driver-set-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($stageDir)
$stageInf = Join-Path $stageDir 'bc250kmd.inf'
[IO.File]::WriteAllText($stageInf, $lab)
$script:StageCalls = 0
foreach ($answer in @($true, $false)) {
    $script:StageAnswer = $answer
    $staged = Invoke-DriverSetStage $stageInf { param($Directory, $Flags)
        Check ($Directory -eq $stageDir -and $Flags -eq 0x40) 'native call gets the directory and INSTALL_AS_SET only' | Out-Host
        $script:StageCalls++; return $script:StageAnswer
    }
    Check ($staged.deferred -and $staged.api_need_reboot -eq $answer) 'both API restart answers retain the shutdown boundary'
}
[IO.File]::WriteAllText((Join-Path $stageDir 'extra.inf'), $lab)
$threw = $false; try { Invoke-DriverSetStage $stageInf { $script:StageCalls++; return $true } } catch { $threw = $true }
Check ($threw -and $script:StageCalls -eq 2) 'extra INF is refused before native staging'
Remove-Item -LiteralPath (Join-Path $stageDir 'extra.inf')
[IO.File]::WriteAllText($stageInf, $rel)
$threw = $false; try { Invoke-DriverSetStage $stageInf { $script:StageCalls++; return $true } } catch { $threw = $true }
Check ($threw -and $script:StageCalls -eq 2) 'forbidden display directive is refused before native staging'
[IO.File]::WriteAllText($stageInf, $lab)
$threw = $false; try { Invoke-DriverSetStage $stageInf { throw 'injected native staging failure' } } catch { $threw = $true }
Check $threw 'native failure produces no successful receipt'
$receipt = [pscustomobject]@{ method = 'di-install-as-set'; inf_sha256 = 'A'; boot_id = 'boot1'; staged = $true }
Check ((Get-DriverSetAction $null 'A' 'boot1') -eq 'stage') 'first install stages'
Check ((Get-DriverSetAction $receipt 'A' 'boot1') -eq 'restart') 'same boot cannot verify or stage twice'
Check ((Get-DriverSetAction $receipt 'A' 'boot2') -eq 'verify-binding') 'new boot checks exact binding without another staging loop'
Check ((Get-DriverSetAction $receipt 'B' 'boot2') -eq 'stage') 'another INF requires its own staging receipt'
Check ((Get-DriverSetAction $receipt 'A' '') -eq 'unknown-boot') 'unknown boot cannot cross the boundary'
$receipt.staged = $false
Check ((Get-DriverSetAction $receipt 'A' 'boot2') -eq 'invalid-receipt') 'an incomplete receipt cannot permit verification'

'BD-227: a driver set that Windows would not bind is refused before the state handoff'
# The INFs differ in their KMD identity line only, as two builds of one release do (tester.32 round 6 lab over round
# 5c lab: both 40.7.216.104, the GPU stayed on the old package and its KMD did not start after the restart).
function New-RankInf([string]$DriverVer, [string]$Kmd) { return ($lab -replace 'DriverVer   = [^\r]+', "DriverVer   = $DriverVer") + "; kmd $Kmd`r`n" }
function Get-InfTextSha([string]$Text) { return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))) -replace '-', '') }
function Get-Rank([string]$Staged, $Bound) { return Get-DriverSetRankDecision -StagedInfText $Staged -StagedInfSha (Get-InfTextSha $Staged) -Bound $Bound }
function New-Bound([string]$Text, [string]$Service = 'bc250kmd', [string]$Inf = 'oem3.inf') {
    return [pscustomobject]@{ service = $Service; ours = ($Service -eq 'bc250kmd'); inf = $Inf; inf_sha256 = $(if ($null -ne $Text) { Get-InfTextSha $Text }); inf_text = $Text }
}
$r5c = New-RankInf '10/23/2026,40.7.216.104' 'CDE214D4'
$r6 = New-RankInf '10/23/2026,40.7.216.104' 'C35DCDD0'
$d = Get-Rank $r6 (New-Bound $r5c)
Check (-not $d.ok -and $d.id -eq 'not-above' -and $d.detail -match 'oem3\.inf' -and $d.detail -match 'uninstall\.cmd') "same DriverVer, another KMD: refused ($($d.detail))"
$d = Get-Rank $r6 (New-Bound $r6)
Check ($d.ok -and $d.id -eq 'ok') 'the same INF as the bound package (a repair of the same build) passes'
$d = Get-Rank $r6 (New-Bound $r6.ToUpperInvariant())
Check (-not $d.ok -and $d.id -eq 'not-above') 'an INF that differs in any byte is another package, also with an equal DriverVer'
$d = Get-Rank (New-RankInf '10/24/2026,40.7.216.104' 'C35DCDD0') (New-Bound $r5c)
Check ($d.ok -and $d.id -eq 'ok') 'a newer DriverVer date with the same version passes'
$d = Get-Rank (New-RankInf '10/23/2026,40.7.216.105' 'C35DCDD0') (New-Bound $r5c)
Check ($d.ok -and $d.id -eq 'ok') 'the same date with a higher version passes'
$d = Get-Rank (New-RankInf '10/22/2026,40.7.216.200' 'C35DCDD0') (New-Bound $r5c)
Check (-not $d.ok -and $d.id -eq 'not-above') 'an older date refuses even with a higher version: PnP compares the date first'
$d = Get-Rank (New-RankInf '10/23/2026,40.7.216.103' 'C35DCDD0') (New-Bound $r5c)
Check (-not $d.ok -and $d.id -eq 'not-above') 'a lower version on the same date (a downgrade) refuses'
$d = Get-Rank (New-RankInf '10/23/2026,40.7.216.9' 'C35DCDD0') (New-Bound (New-RankInf '10/23/2026,40.7.216.10' 'CDE214D4'))
Check (-not $d.ok) 'versions compare as numbers, not as text (40.7.216.9 is below 40.7.216.10)'
$d = Get-Rank $r6 (New-Bound $null 'BasicDisplay' $null)
Check ($d.ok -and $d.id -eq 'ok') 'a GPU on another driver (Microsoft Basic Display) passes'
$d = Get-Rank $r6 $null
Check ($d.ok -and $d.id -eq 'ok') 'a GPU with no driver service passes'
$d = Get-Rank $r6 (New-Bound $null)
Check (-not $d.ok -and $d.id -eq 'unreadable') 'our service with an unreadable bound INF refuses (it is not known that Windows moves the GPU)'
$d = Get-Rank $r6 ([pscustomobject]@{ service = 'bc250kmd'; ours = $true; inf = $null; inf_sha256 = $null; inf_text = $null })
Check (-not $d.ok -and $d.id -eq 'unreadable') 'our service with no published INF name refuses'
$d = Get-Rank $r6 (New-Bound ($r5c -replace 'DriverVer   = [^\r]+\r\n', ''))
Check (-not $d.ok -and $d.id -eq 'unreadable') 'a bound INF without a DriverVer refuses'
$d = Get-Rank ($r6 -replace 'DriverVer   = [^\r]+\r\n', '') (New-Bound $r5c)
Check (-not $d.ok -and $d.id -eq 'unreadable') 'a package INF without a DriverVer refuses'
# The wiring in install.ps1: the preflight runs the check before any change, -Force does not pass it, and the freeze of
# the state handoff reads it again first.
$installText = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
Check ($installText -match "(?m)^\s+\`$rank = Test-DriverSetRank -InstallState \`$st\r?\n\s+if \(\`$rank\) \{ Add-Check 'driver package rank' \`$\(if \(\`$rank\.ok\) \{ 'ok' \} else \{ 'fail' \}\)") 'the preflight adds the rank check, as fail when it refuses, with no -Force exception'
$freeze = $installText.IndexOf("Invoke-Change 'freeze state writers and publish the protected state handoff before INF staging'")
$guard = $installText.IndexOf('if ($rank -and -not $rank.ok) { throw "driver package rank: $($rank.detail)" }')
$compare = $installText.IndexOf('Save-DriverSettingsComparison -State $state')
Check ($freeze -gt 0 -and $guard -gt $freeze -and $compare -gt $guard) 'the freeze reads the rank again and throws before it saves or freezes anything'
Check ($installText -match "(?m)^function Test-DriverSetRank\(\`$InstallState\) \{\r?\n\s+if \(\`$legacyRollback -or -not \`$script:Device -or \`$action\.action -notin @\('install', 'upgrade', 'repair', 'resume'\)\) \{ return \`$null \}") 'only a run that installs checks the rank, and a rollback removes the other ranks itself'
Check ($installText -match "\(Get-DriverSetAction \`$receipt \`$sha \(\[string\]\(Get-BootIdentity\)\.boot_id\)\) -ne 'stage'\) \{ return \`$null \}") 'a driver set that is staged already is left to the binding check after the restart'

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

'install inputs across the argument-free continuation (the RunOnce continuation passes no install inputs)'
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
# A newer package takes over a tester.10 phase 1 (test signing pending, or active with phase 2 not begun) with its own
# arguments: the state names it from then on, so its argument-free continuation after the driver package restart
# restores what it saved. The phase is not touched: test signing is still finished first.
foreach ($ph in 'testsigning-pending', 'testsigning-active') {
    $st = [pscustomobject]@{ schema = 1; phase = $ph; package_version = '0.7.198.100-tester.10'; firmware_source_dir = 'C:\old-fw'; command_line_parameters = [pscustomobject]@{ DpmMaxMHz = 1500 } } | ConvertTo-Json | ConvertFrom-Json
    $ia = Get-InstallAction -State $st -PackageVersion $v11 -InstalledVersion '0.7.198.100-tester.10' -Repair $false
    $in = Get-InstallInputs @{ FirmwareDir = 'F:\fw11'; CuMode = 24; NoControlApp = [switch]$true } $st $v11
    Check (($ia.action -eq 'install') -and ((Format-Inputs $in) -eq 'fw=F:\fw11 params=CuMode=24 switches=NoControlApp') -and (@($in.restored).Count -eq 0)) "takeover at ${ph}: action install, only the new package's own arguments (nothing from tester.10's run)"
    $was = Set-InstallPackage $st $v11
    Save-InstallInputs $st $in
    Check (($was -eq '0.7.198.100-tester.10') -and ($st.package_version -eq $v11) -and ($st.phase -eq $ph)) "takeover at ${ph}: the state names the new package, the phase stays (test signing first)"
    if ($ph -eq 'testsigning-pending') {
        # The run after the test-signing restart (this package's, from RunOnce) has no arguments either.
        $mid = Get-InstallInputs $none (Step-State $st 'testsigning-pending') $v11
        Check ((Format-Inputs $mid) -eq 'fw=F:\fw11 params=CuMode=24 switches=NoControlApp') "takeover at ${ph}: the run after the test-signing restart restores them: $(Format-Inputs $mid)"
    }
    $next = Get-InstallInputs $none (Step-State $st 'driver-pending-restart') $v11
    Check ((Format-Inputs $next) -eq 'fw=F:\fw11 params=CuMode=24 switches=NoControlApp') "takeover at ${ph}, then the driver package restart without arguments: $(Format-Inputs $next)"
}
Check ($null -eq (Set-InstallPackage ([pscustomobject]@{ phase = 'new' }) $v11)) 'a state without a package version: nothing to take over'
Check ($null -eq (Set-InstallPackage ([pscustomobject]@{ phase = 'testsigning-active'; package_version = $v11 }) $v11)) 'the same package: nothing to take over'

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

# BD-080: the logon tasks, as the plan install.ps1 registers (common.ps1 Get-LogonTaskPlan, Test-LogonTaskPlan).
# Nothing here registers, starts or reads a scheduled task: the plan is data, and the rules are read from it. The
# defect was the principal of the one task that existed, the Administrators group, which never runs at the logon of
# a standard user, so that user's machine counted unconfirmed starts until the driver refused to start.
'BD-080: the logon tasks'
$confirmScript = 'C:\Program Files\amdgpu-wddm\tools\start-confirm.ps1'
$windowScript = 'C:\Program Files\amdgpu-wddm\tools\start-window.ps1'
$windowHost = 'C:\Program Files\amdgpu-wddm\control\amdgpu_wddm_control.exe'
$plan = $null
if (Get-Command Get-LogonTaskPlan -ErrorAction SilentlyContinue) { $plan = @(Get-LogonTaskPlan -ConfirmScript $confirmScript -WindowScript $windowScript -WindowHost $windowHost) }
Check ($null -ne $plan) 'common.ps1 states the logon tasks as a plan (Get-LogonTaskPlan)'
if ($plan) {
    $problems = @(Test-LogonTaskPlan $plan)
    Check ($problems.Count -eq 0) "the plan holds every rule$(if ($problems.Count) { ': ' + ($problems -join '; ') } else { '' })"
    $c = @($plan | Where-Object { $_.role -eq 'confirm' })[0]
    $w = @($plan | Where-Object { $_.role -eq 'window' })[0]
    Check (($c.principal.id -eq 'S-1-5-18') -and ($c.principal.logon_type -eq 'ServiceAccount') -and ($c.principal.run_level -eq 'Highest')) "the confirmation runs as Local System with the highest run level: $($c.principal.id), $($c.principal.logon_type), $($c.principal.run_level)"
    Check ((@($plan | Where-Object { $_.trigger.kind -eq 'at-logon' -and -not $_.trigger.user }).Count) -eq $plan.Count) 'both tasks trigger at the logon of any user (no account on the trigger)'
    Check (($w.principal.id -eq 'S-1-5-32-545') -and ($w.principal.run_level -eq 'Limited')) "the window runs with the logged-on user's own rights: $($w.principal.id), $($w.principal.run_level)"
    Check (($c.arguments -match '(?i)\s-NoWindow(\s|$)') -and ($c.arguments -like "*`"$confirmScript`"*")) "the confirmation runs $($c.execute) headless over the installed script: $($c.arguments)"
    # M899: the window task runs the control application, a windows program, which starts the script with no console
    # window. powershell.exe in the task gave the person a Windows Terminal window at sign-in.
    Check (($w.execute -eq $windowHost) -and ($w.arguments -eq '--start-window -Seconds 120') -and ($w.script -eq $windowScript) -and (-not $w.console)) "the window runs the control application of this install, which starts its own script with no console window: $($w.execute) $($w.arguments)"
    # An install without the control application (-NoControlApp) keeps powershell.exe, and the plan says why.
    $plainPlan = @(Get-LogonTaskPlan -ConfirmScript $confirmScript -WindowScript $windowScript)
    $pw = @($plainPlan | Where-Object { $_.role -eq 'window' })[0]
    $plainProblems = @(Test-LogonTaskPlan $plainPlan)
    Check (($plainProblems.Count -eq 0) -and ($pw.execute -eq 'powershell.exe') -and ($pw.arguments -like "*`"$windowScript`"*") -and ($pw.arguments -notmatch '(?i)start-confirm') -and [string]$pw.console) "without the control application the window runs powershell.exe over its own script and the plan says why: $($pw.console)$(if ($plainProblems.Count) { ' (' + ($plainProblems -join '; ') + ')' } else { '' })"
    # The PE reader of the host rule: a windows program, a console program, and files that are not programs.
    $sys = Join-Path $env:SystemRoot 'System32'
    $txt = Join-Path $WorkRoot ('pe-subsystem-' + [guid]::NewGuid().ToString('N') + '.txt')
    [void](New-Item -ItemType Directory -Force -Path $WorkRoot)
    [IO.File]::WriteAllText($txt, 'MZ is not enough')
    try {
        $readings = @((Get-PeSubsystem (Join-Path $env:SystemRoot 'explorer.exe')), (Get-PeSubsystem (Join-Path $sys 'cmd.exe')), (Get-PeSubsystem $txt), (Get-PeSubsystem (Join-Path $sys 'no-such-file.exe')))
    } finally { Remove-Item -LiteralPath $txt -Force -ErrorAction SilentlyContinue }
    Check (($readings[0] -eq 2) -and ($readings[1] -eq 3) -and ($null -eq $readings[2]) -and ($null -eq $readings[3])) "Get-PeSubsystem: explorer.exe $($readings[0]), cmd.exe $($readings[1]), a text file '$($readings[2])', no file '$($readings[3])' (expected 2, 3, none, none)"
    Check ((@($plan | ForEach-Object { [string]$_.name } | Sort-Object -Unique).Count -eq 2) -and ($plan[0].name -eq $script:TaskName) -and ($plan[1].name -eq $script:WindowTaskName)) "two tasks, both named after the release: '$($plan[0].name)', '$($plan[1].name)'"
    # The negative controls: each one is the plan with one rule broken, and every one of them must be reported.
    function Copy-Plan($Source) {
        $out = @()
        foreach ($t in @($Source)) {
            $r = [ordered]@{}
            foreach ($k in $t.Keys) { if ($t[$k] -is [System.Collections.IDictionary]) { $r[$k] = [ordered]@{}; foreach ($k2 in $t[$k].Keys) { $r[$k][$k2] = $t[$k][$k2] } } else { $r[$k] = $t[$k] } }
            $out += $r
        }
        return $out
    }
    $controls = @(
        @{ name = 'the principal of the defect: the Administrators group'; break = { param($p) $p[0].principal = [ordered]@{ kind = 'group'; id = 'S-1-5-32-544'; logon_type = 'Group'; run_level = 'Highest' } }; says = 'S-1-5-32-544' }
        @{ name = 'a trigger bound to one account'; break = { param($p) $p[0].trigger.user = 'BC250-A\bc250' }; says = 'every logon' }
        @{ name = 'the confirmation without an administrator token'; break = { param($p) $p[0].principal.run_level = 'Limited' }; says = 'not an administrator' }
        @{ name = 'the confirmation with a window in session 0'; break = { param($p) $p[0].arguments = $p[0].arguments -replace '(?i)\s-NoWindow', '' }; says = 'headless' }
        @{ name = 'the confirmation read-only'; break = { param($p) $p[0].arguments = $p[0].arguments + ' -Probe' }; says = 'confirm no start' }
        @{ name = 'the fallback hold moved from the command line'; break = { param($p) $p[0].arguments = $p[0].arguments + ' -FallbackHoldSeconds 5' }; says = 'boot-loop fallback' }
        @{ name = 'the window asking for rights'; break = { param($p) $p[1].principal.run_level = 'Highest' }; says = 'needs no rights' }
        @{ name = 'the window for administrators only'; break = { param($p) $p[1].principal = [ordered]@{ kind = 'group'; id = 'S-1-5-32-544'; logon_type = 'Group'; run_level = 'Limited' } }; says = 'S-1-5-32-544' }
        @{ name = 'a task not named after the release'; break = { param($p) $p[1].name = 'start window' }; says = "uninstaller's sweep" }
        @{ name = 'a task without a time limit'; break = { param($p) $p[0].minutes = 0 }; says = 'no execution time limit' }
        @{ name = 'only the window registered'; break = { param($p) $p[0].role = 'window' }; says = '0 confirmation tasks' }
        @{ name = 'the window through powershell.exe with no reason (M899)'; break = { param($p) $p[1].execute = 'powershell.exe'; $p[1].arguments = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$windowScript`" -Seconds 120"; $p[1].console = $null }; says = 'console program' }
        @{ name = 'the host not asked for the window'; break = { param($p) $p[1].arguments = '-Seconds 120' }; says = '--start-window' }
        @{ name = 'the host of another install'; break = { param($p) $p[1].execute = 'D:\Other\amdgpu-wddm\control\amdgpu_wddm_control.exe' }; says = 'different installs' }
        @{ name = 'a console program as the host'; break = { param($p) $p[1].execute = (Join-Path $env:SystemRoot 'System32\cmd.exe'); $p[1].script = (Join-Path $env:SystemRoot 'tools\start-window.ps1') }; says = 'PE subsystem 3' }
    )
    foreach ($ctl in $controls) {
        $broken = Copy-Plan $plan
        & $ctl.break $broken
        $said = @(Test-LogonTaskPlan $broken)
        Check (($said.Count -ge 1) -and (($said -join '; ') -like "*$($ctl.says)*")) "negative control, $($ctl.name): refused ($(if ($said.Count) { $said -join '; ' } else { 'nothing said' }))"
    }
}
# install.ps1 registers that plan and nothing else: one principal in the source, built from the plan, and the group
# of the defect nowhere in the installer.
foreach ($f in @('install.ps1', 'common.ps1', 'uninstall.ps1')) {
    $src = [IO.File]::ReadAllText((Join-Path $Installer $f))
    Check ($src -notmatch 'New-ScheduledTaskPrincipal[^\r\n]*S-1-5-32-544') "$f registers no task for the Administrators group (BD-080)"
}
$installSrc = [IO.File]::ReadAllText((Join-Path $Installer 'install.ps1'))
Check ($installSrc -match 'Get-LogonTaskPlan') 'install.ps1 registers the tasks from the plan'
$principals = @([regex]::Matches($installSrc, 'New-ScheduledTaskPrincipal[^\r\n]*'))
Check (($principals.Count -ge 1) -and (@($principals | Where-Object { $_.Value -notmatch '\$\w+\.principal\.' }).Count -eq 0) -and (@($principals | Where-Object { $_.Value -match 'S-1-' }).Count -eq 0)) "install.ps1 takes every principal from the plan and names no account of its own ($($principals.Count) call(s))"
# The trigger is the other half of the defect: a task that runs at the logon of one account only is BD-080 again,
# whatever its principal says. The installer reads the plan's trigger and refuses any plan it cannot register as
# "at the logon of any user", and it holds the plan against Test-LogonTaskPlan before it registers anything, so the
# rules the controls above exercise are not a property of the test alone.
Check ($installSrc -match '(?s)Test-LogonTaskPlan[^\r\n]*\r?\n[^\r\n]*throw') 'install.ps1 runs the rules over the plan it registers and stops the install on a plan that breaks them'
$planCall = [regex]::Match($installSrc, '(?s)Get-LogonTaskPlan.{0,2600}?Register-ScheduledTask[^\r\n]*')
Check ($planCall.Success -and ($planCall.Value -match '\$logonTask\.trigger\.kind') -and ($planCall.Value -match '\$logonTask\.trigger\.user')) "install.ps1 reads the plan's trigger: a plan that is not at-logon, or that binds the trigger to one account, is refused and not registered as this one"
$triggers = @([regex]::Matches($installSrc, '(?m)^(?!\s*#)[^\r\n]*New-ScheduledTaskTrigger[^\r\n]*'))
Check (($triggers.Count -eq 1) -and ($triggers[0].Value -match '(?i)-AtLogOn') -and ($triggers[0].Value -notmatch '(?i)-User')) "install.ps1 registers one trigger, at any logon and bound to no account: $(if ($triggers.Count) { ($triggers | ForEach-Object { $_.Value.Trim() }) -join ' | ' } else { 'no call at all' })"

# BD-080: the two scripts behind those tasks. The confirmation decides headless (one poll function, reached from one
# place), and the script of the user's session is the unprivileged half: it must not call the driver at all, because
# a standard user's CONFIRM would be refused by the kernel anyway.
'BD-080: the scripts behind the logon tasks'
$confirmFile = Join-Path $Installer 'start-confirm.ps1'
$windowFile = Join-Path $Installer 'start-window.ps1'
$confirmText = [IO.File]::ReadAllText($confirmFile)
Check (Test-Path -LiteralPath $windowFile) "the script of the user's session ships with the installer ($windowFile)"
Check ($confirmText -match '(?m)^param\([^\r\n]*\[switch\]\$NoWindow') 'start-confirm.ps1 still takes -NoWindow, which the registered task passes'
Check ((@([regex]::Matches($confirmText, '(?m)^function Invoke-Poll')).Count -eq 1) -and (@([regex]::Matches($confirmText, '\$step = Invoke-Poll')).Count -eq 1)) 'one poll function, called from one place (guard and DPM coupling unchanged)'
Check (@([regex]::Matches($confirmText, '(?m)^\s+\$step = Invoke-Tick')).Count -eq 1) 'one run, the headless loop, through the tick'
Check ($confirmText -match 'Confirm-Health' -and $confirmText -match 'Complete-WithFallback' -and $confirmText -match "Invoke-Cli @\('confirm'\)") 'the confirmation keeps the KMD start-health confirm and the boot-loop fallback'
# The blue start window is gone. Neither script makes a window of its own any more: the KMD's QUIET takes an idle GPU
# desktop as it is (driver/kmd/start_health.c), and the one window of the logon is the sign-in notice of the control
# application, which shows only when the driver is not running.
$windowText = $(if (Test-Path -LiteralPath $windowFile) { [IO.File]::ReadAllText($windowFile) } else { '' })
foreach ($t in @(@{ n = 'start-confirm.ps1'; s = $confirmText }, @{ n = 'start-window.ps1'; s = $windowText })) {
    $made = @('Windows.Forms', 'System.Drawing', 'TopMost', 'ShowDialog', 'New-StartWindow', 'Format-StartWindowText', 'ShowStartWindow') | Where-Object { $t.s -cmatch [regex]::Escape($_) }
    Check ($made.Count -eq 0) "$($t.n) makes no window of its own$(if ($made.Count) { ": $($made -join ', ')" } else { '' })"
}
$coreText = [IO.File]::ReadAllText((Join-Path $Installer 'start-confirm-core.ps1'))
Check ($coreText -match '\(\$Reading\.flags -band 16\) -eq 16') 'the confirmation takes the KMD''s QUIET (flag 16) in place of a fresh flip, as the KMD does'
if ($windowText) {
    $kmd = @('bc250control', 'bc250kmd_cli', 'Bc250StartHealth', 'CurrentControlSet', 'New-ItemProperty', 'Set-ItemProperty', 'Microsoft.Win32.Registry', 'HKLM', 'HKCU', 'Add-Type -TypeDefinition') | Where-Object { $windowText -cmatch [regex]::Escape($_) }
    Check ($kmd.Count -eq 0) "the script of the user's session opens no handle to the driver and reads or writes no registry value$(if ($kmd.Count) { ": $($kmd -join ', ')" } else { '' })"
    # The sign-in notice: this task starts it, because it is the task that already runs in the
    # session of the person who just signed in. The script never waits for it, so the task completes inside its own
    # time limit and does not stop the notice.
    Check ($windowText -cmatch '--start-notice') 'the script starts the sign-in notice of the control application'
    Check ($windowText -cmatch 'Start-Process[^\r\n]*--start-notice' -and $windowText -notmatch 'Start-Process[^\r\n]*-Wait') 'the script does not wait for the notice'
    # No startup information on that start, above all no -WindowStyle. Windows ignores the first ShowWindow call of
    # a process whose launcher passed STARTUPINFO, and that first call is the only show a WinForms window makes, so
    # a start with -WindowStyle Hidden (the style every headless smoke of this exe in the tree uses) would create
    # the notice and never put it on the screen.
    Check ($windowText -notmatch 'Start-Process[^\r\n]*--start-notice[^\r\n]*-WindowStyle' -and
        $windowText -notmatch '-WindowStyle[^\r\n]*--start-notice') 'the notice is not started with a window style, so its window is actually shown'
    Check ($windowText -cmatch 'Test-Path -LiteralPath \$exe -PathType Leaf') 'the script starts the notice only when the control application is there'
    Check ($windowText -match '(?m)^param\([^\r\n]*\[int\]\$Seconds') 'the script still takes -Seconds, which the host passes'
    # The script runs in a session this project never watches, and it writes no file (a standard user may write
    # nowhere of ours), so its exit code is its whole report. A catch that exits 0 would make a refused start of the
    # notice indistinguishable from a started one.
    Check ($windowText -notmatch '(?s)catch\s*\{[^{}]*exit\s+0') 'the script does not report success from its catch'
    Check ($windowText -cmatch '(?s)try \{.{0,2000}--start-notice.{0,200}\r?\n\} catch \{\s*exit 2\s*\}') 'a notice that Windows refuses to start exits 2, which the task keeps as LastTaskResult, and fails nothing else'
    # The confirmation runs as Local System and can read that result, which is the only place the user's half
    # becomes observable at all.
    Check ($confirmText -match "(?m)^\`$windowTaskName = '$([regex]::Escape($script:WindowTaskName))'") "start-confirm.ps1 names the same task as the plan ('$($script:WindowTaskName)')"
    Check ((@([regex]::Matches($confirmText, '(?m)^function Format-WindowTask')).Count -eq 1) -and ($confirmText -match 'Get-ScheduledTaskInfo')) 'start-confirm.ps1 reads that task (state, last run, last result)'
    Check ($confirmText -match '(?m)^Write-StartConfirmLog "exit [^\r\n]*Format-WindowTask') 'the exit line of every logon reports what that task did, so a half that never ran is in the log'
}

if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'session checks: all checks passed'
exit 0
