# Host test of the BD-060 rules in common.ps1: the INF Reboot directive (the GPU changes driver at the next restart,
# not under the running desktop), the pnputil outcomes, the stale BD-059 session marker after an in-place device
# restart, and verify's "DWM restarted in this session". Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-session-checks.ps1 [-Installer <package>\installer] [-Inf <packaged INF>]
# -Inf: the INF of a built package, which must carry the directive. The marker cases run against a scratch key,
# HKCU:\Software\amdgpu-wddm-installer-test-session, removed at the end. Nothing under HKLM is read or written; the
# logon time and the DWM start of this computer's own session are read only.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$Inf)
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Split-Lines([string]$Text) { return , ($Text -split "`r?`n") }

# The shape of driver\kmd\bc250kmd.inf: [Manufacturer] -> Models.NTamd64 -> Bc250_Install, commented-out lines that
# the lab build switches on, and a service section that is not an install section.
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

'pnputil outcomes'
$o = Get-DriverPackageOutcome 3010
Check ($o.ok -and $o.deferred -and -not $o.inPlace) '3010: installed, the GPU changes at the restart'
$o = Get-DriverPackageOutcome 0
Check ($o.ok -and $o.inPlace -and -not $o.deferred) '0: installed on a device that was not started (in place)'
$o = Get-DriverPackageOutcome 259
Check ($o.ok -and -not $o.inPlace -and -not $o.deferred) '259: already in the driver store, no device update'
Check (-not (Get-DriverPackageOutcome 5).ok) 'any other exit code fails'

'BD-059 session marker after an in-place device restart (HKCU scratch key)'
$key = 'HKCU:\Software\amdgpu-wddm-installer-test-session'
Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue
try {
    Initialize-RegistryKey $key
    Check (-not (Test-StaleInteropMarker $key)) 'no marker: nothing to remove'
    New-ItemProperty -LiteralPath $key -Name InteropSession -Value 175 -PropertyType DWord -Force | Out-Null
    Check (-not (Test-StaleInteropMarker $key)) 'a marker without this boot''s record (an earlier boot''s death) stays for the KMD'
    Initialize-RegistryKey "$key\InteropBoot"
    New-ItemProperty -LiteralPath "$key\InteropBoot" -Name Marked -Value 0 -PropertyType DWord -Force | Out-Null
    Check (-not (Test-StaleInteropMarker $key)) 'InteropBoot Marked 0: not this boot''s marker'
    New-ItemProperty -LiteralPath "$key\InteropBoot" -Name Marked -Value 1 -PropertyType DWord -Force | Out-Null
    Check (Test-StaleInteropMarker $key) 'marker plus InteropBoot Marked 1: the stopped instance''s marker of this boot (stale)'
    Remove-ItemProperty -LiteralPath $key -Name InteropSession
    Check (-not (Test-StaleInteropMarker $key)) 'record of this boot without a marker: nothing to remove'
} finally { Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue }
Check (-not (Test-Path -LiteralPath $key)) 'scratch key removed'

'DWM restarted in this session'
$logon = [datetime]::SpecifyKind([datetime]'2026-10-03T20:33:40', 'Utc')
$f = Get-DwmRestartFinding -SessionId 1 -LogonUtc $logon -DwmStartUtc @($logon.AddSeconds(-9))
Check (($f.state -eq 'first') -and ($f.detail -match '^no: the DWM of session 1 started at 2026-10-03 20:33:31Z, before the logon at 2026-10-03 20:33:40Z$')) "first DWM of the session: $($f.detail)"
$f = Get-DwmRestartFinding -SessionId 1 -LogonUtc $logon -DwmStartUtc @($logon.AddMinutes(3))
Check (($f.state -eq 'restarted') -and ($f.detail -match '^yes: .+ started at 2026-10-03 20:36:40Z, after the logon at 2026-10-03 20:33:40Z\. .+ignore mouse clicks until the next restart\. This is Windows behaviour, not a driver failure\. Restart the computer\.$')) 'a DWM started after the logon: restarted, with the remedy'
$f = Get-DwmRestartFinding -SessionId 1 -LogonUtc $logon -DwmStartUtc @($logon.AddSeconds(-9), $logon.AddMinutes(3))
Check ($f.state -eq 'restarted') 'two DWMs, the later one after the logon: restarted'
Check ((Get-DwmRestartFinding -SessionId 0 -LogonUtc $logon -DwmStartUtc @($logon)).state -eq 'none') 'session 0 (a service or SSH shell): not judged'
Check ((Get-DwmRestartFinding -SessionId 1 -LogonUtc $null -DwmStartUtc @($logon)).state -eq 'none') 'nobody logged on: not judged'
Check ((Get-DwmRestartFinding -SessionId 1 -LogonUtc $logon -DwmStartUtc @()).state -eq 'none') 'no DWM in the session: not judged'

# This computer's own session, read only: the WTS logon time must come back for a session with a user, and be in
# the past. The finding is printed, not judged (the development PC's DWM may have been restarted).
$sid = (Get-Process -Id $PID).SessionId
$t = Get-SessionLogonUtc $sid
if ($sid -eq 0) { "  (session 0: logon time $t; skipped)" }
else {
    Check (($null -ne $t) -and ($t -lt [DateTime]::UtcNow) -and ($t.Kind -eq 'Utc')) "WTS logon time of session $sid read: $($t.ToString('u'))"
    $starts = Get-SessionDwmStartUtc $sid
    Check ($starts.Count -ge 1) "DWM start time of session $sid read without elevation: $(@($starts | ForEach-Object { $_.ToString('u') }) -join ', ')"
    "  (this computer: $((Get-DwmRestartFinding -SessionId $sid -LogonUtc $t -DwmStartUtc $starts).detail))"
}

if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'session checks: all checks passed'
exit 0
