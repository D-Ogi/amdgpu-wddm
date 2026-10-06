# Host test of the upgrade rule for registry defaults (common.ps1, Get-RegistryDefaultPlan): a new default replaces
# only what the previous installer wrote, a tester's value stays, the command line and installer-owned values are
# written. Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-registry-defaults.ps1 [-Installer <package>\installer]
# The write and read-back run against a scratch key, HKCU:\Software\amdgpu-wddm-installer-test, removed at the end.
# Nothing under HKLM is read or written.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'))
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
function Get-Decision($Plan, [string]$Name) { return (@($Plan) | Where-Object { $_.name -eq $Name } | Select-Object -First 1) }

$table = Get-Content -LiteralPath (Join-Path $Installer 'registry-defaults.json') -Raw | ConvertFrom-Json
$legacy = $table.legacy_applied
# A later release's table: the desktop on the GPU route and a new Deny entry.
$next = $table.defaults | ConvertTo-Json -Depth 6 | ConvertFrom-Json
$next.desktop_router.DwmForceCpu = 0
# A release whose default kept the desktop on the CPU route (tester.1 to tester.8).
$old = $table.defaults | ConvertTo-Json -Depth 6 | ConvertFrom-Json
$old.desktop_router.DwmForceCpu = 1
$next.app_router.Deny = @('witcher3.exe', 'other.exe')

'fresh install: every value is new'
$plan = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{}
Check (@($plan | Where-Object { $_.decision -ne 'set' }).Count -eq 0) "all $(@($plan).Count) parameters 'set'"
Check (((Get-Decision $plan 'DpmMaxMHz').value -eq 1500) -and (Get-Decision $plan 'DpmMaxMHz').write) 'DpmMaxMHz 1500 written'

'upgrade from tester.7 (no record): installer values follow the new defaults, tester values stay'
$cur = @{ DwmForceCpu = 1; RequireKmdSwitches = 0 }
$plan = Get-RegistryDefaultPlan -Defaults $next.desktop_router -Previous $legacy.desktop_router -Current $cur
$d = Get-Decision $plan 'DwmForceCpu'
Check (($d.decision -eq 'update') -and ($d.value -eq 0) -and $d.write) "DwmForceCpu 1 (written by tester.7) -> 0: $($d.decision)"
$d = Get-Decision $plan 'RequireKmdSwitches'
Check (($d.decision -eq 'kept') -and ($d.value -eq 0) -and -not $d.write) "RequireKmdSwitches 0 (tester) kept: $($d.decision)"
$cur = @{ DwmForceCpu = 0 }
$plan = Get-RegistryDefaultPlan -Defaults $old.desktop_router -Previous $legacy.desktop_router -Current $cur
$d = Get-Decision $plan 'DwmForceCpu'
Check (($d.decision -eq 'kept') -and -not $d.write) "DwmForceCpu 0 set by the tester under default 1: $($d.decision)"

$p = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMaxMHz = 1200; DpmMode = 1; EnableCddDwmInterop = 0 }
Check ((Get-Decision $p 'DpmMaxMHz').decision -eq 'kept') 'DpmMaxMHz 1200 (tester) kept'
Check ((Get-Decision $p 'DpmMode').decision -eq 'same') 'DpmMode 1 unchanged'
Check ((Get-Decision $p 'EnableCddDwmInterop').decision -eq 'kept') 'EnableCddDwmInterop 0 (tester) kept'
Check ((Get-Decision $p 'EnableGpuPresentBlit').decision -eq 'set') 'EnableGpuPresentBlit absent: set'

'the driver''s own safety closures (BD-069): a repair reopens them, another install reports them'
# What the lab had after four 0x116 boots: the KMD wrote both interop switches 0 with InteropClosedReason 4
# (unclean), and the DPM guard wrote DpmMode 0 with DpmClosedReason 4 and DpmLastReason 4 (KMD 0.7.208.1 writes
# both; a driver before it wrote DpmLastReason alone, which the legacy case below covers).
$afterClose = @{ EnableGpuPresentBlit = 0; EnableCddDwmInterop = 0; InteropClosedReason = 4; DpmMode = 0; DpmClosedReason = 4; DpmLastReason = 4; DpmMaxMHz = 1500 }
$p = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $afterClose
foreach ($n in 'EnableGpuPresentBlit', 'EnableCddDwmInterop') {
    $d = Get-Decision $p $n
    Check (($d.decision -eq 'driver-closed') -and ($d.value -eq 0) -and -not $d.write -and ($d.closure_record -eq 'InteropClosedReason') -and ($d.closure_code -eq 4) -and ($d.closure_reason -eq 'unclean') -and $d.closure) "$n 0 with InteropClosedReason 4, no repair: $($d.decision), reported as '$($d.closure)'"
}
$d = Get-Decision $p 'DpmMode'
Check (($d.decision -eq 'driver-closed') -and ($d.value -eq 0) -and -not $d.write -and ($d.closure_record -eq 'DpmClosedReason') -and ($d.closure_code -eq 4)) "DpmMode 0 with DpmClosedReason 4 (the DPM guard), no repair: $($d.decision)"
$text = Format-RegistryPlan $p
Check (($text -match 'EnableGpuPresentBlit=0 KEPT \(the driver closed the GPU desktop path after a boot that ended with the path in use, InteropClosedReason 4 unclean; not a setting of the tester; default 1; remedy: install\.cmd -Repair\)') -and ($text -match 'DpmMode=0 KEPT \(the driver went back to the base clock .*DpmClosedReason 4 unclean')) 'the plan text names the closure, its reason code and the remedy, not the tester'
$p = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $afterClose -Reopen
foreach ($n in 'EnableGpuPresentBlit', 'EnableCddDwmInterop') {
    $d = Get-Decision $p $n
    Check (($d.decision -eq 'reopened') -and ($d.value -eq 1) -and $d.write -and (@($d.clear) -contains 'InteropClosedReason')) "$n reopened by a repair: $($d.decision) -> $($d.value), clears $(@($d.clear) -join ',')"
}
$d = Get-Decision $p 'DpmMode'
Check (($d.decision -eq 'reopened') -and ($d.value -eq 1) -and $d.write -and (@($d.clear) -contains 'DpmClosedReason')) "DpmMode reopened by a repair: $($d.decision) -> $($d.value), clears $(@($d.clear) -join ',')"
Check ((Format-RegistryPlan $p) -match 'EnableCddDwmInterop 0 -> 1 \(reopened by this repair: the driver closed the GPU desktop path after a boot that ended with the path in use, InteropClosedReason 4 unclean\)') 'the plan text of a repair names what it reopens and why'
# A value the tester set by hand has no record of a closure: kept, with a repair too. DpmLastReason 1 is what every
# start writes when it reads DpmMode 0, so it is no record of a closure by itself.
$byHand = @{ EnableCddDwmInterop = 0; DpmMode = 0; DpmLastReason = 1 }
foreach ($reopen in $false, $true) {
    $p = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $byHand -Reopen:$reopen
    Check (((Get-Decision $p 'EnableCddDwmInterop').decision -eq 'kept') -and ((Get-Decision $p 'DpmMode').decision -eq 'kept')) "set by the tester (no record; DpmLastReason 1 = DpmMode 0 as asked), repair $($reopen): both kept"
}
# Both records survive every later start (KMD 0.7.208.1 for DpmClosedReason), so the lab's own case reopens both
# halves one boot after the closure, where DpmLastReason already says 1 (not requested).
$oneStartLater = @{ EnableGpuPresentBlit = 0; EnableCddDwmInterop = 0; InteropClosedReason = 4; DpmMode = 0; DpmClosedReason = 4; DpmLastReason = 1 }
$p = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $oneStartLater
$d = Get-Decision $p 'DpmMode'
Check (($d.decision -eq 'driver-closed') -and ($d.closure_record -eq 'DpmClosedReason') -and ($d.closure_code -eq 4) -and ($d.closure_reason -eq 'unclean')) "one start after the fallback, DpmLastReason 1: the record still names it: $($d.decision) ($($d.closure_reason))"
$p = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $oneStartLater -Reopen
Check ((@($p | Where-Object { $_.decision -eq 'reopened' }).Count -eq 3) -and ((Get-Decision $p 'DpmMode').value -eq 1) -and (@((Get-Decision $p 'DpmMode').clear) -contains 'DpmClosedReason')) 'a start after the closure: a repair reopens both switches and DpmMode, and clears both records'
# A driver before KMD 0.7.208.1 leaves DpmLastReason alone, so it is read instead, with its one-boot limit: the
# fallback's own boot is recognised, and the start after it (DpmLastReason 1, above) is not.
$legacyKmd = @{ EnableGpuPresentBlit = 0; EnableCddDwmInterop = 0; InteropClosedReason = 4; DpmMode = 0; DpmLastReason = 4 }
$d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $legacyKmd) 'DpmMode'
Check (($d.decision -eq 'driver-closed') -and ($d.closure_record -eq 'DpmLastReason') -and ($d.closure_code -eq 4)) "a driver with no DpmClosedReason, DpmLastReason 4: $($d.decision) from $($d.closure_record)"
$d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $legacyKmd -Reopen) 'DpmMode'
Check (($d.decision -eq 'reopened') -and ($d.value -eq 1) -and -not $d.PSObject.Properties['clear']) "that one reopened by a repair: $($d.decision) -> $($d.value), and DpmLastReason stays with the start that owns it"
# A record next to a value that is not the one the driver writes: the value decides, not the record.
$p = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ EnableGpuPresentBlit = 1; InteropClosedReason = 4; DpmMode = 1; DpmClosedReason = 4; DpmLastReason = 4; DpmMaxMHz = 1200 } -Reopen
Check (((Get-Decision $p 'EnableGpuPresentBlit').decision -eq 'same') -and ((Get-Decision $p 'DpmMode').decision -eq 'same') -and ((Get-Decision $p 'DpmMaxMHz').decision -eq 'kept')) 'a record with the value already open: nothing to reopen (same), and the rest judged as before'
# Every reason the DPM guard persists (the three callers of driver/kmd/dpm.c PersistFallback), and the reasons that
# are not the guard's.
foreach ($record in 'DpmClosedReason', 'DpmLastReason') {
    foreach ($case in @{ code = 3; name = 'unconfirmed' }, @{ code = 4; name = 'unclean' }, @{ code = 8; name = 'smu-error' }) {
        $d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; $record = $case.code }) 'DpmMode'
        Check (($d.decision -eq 'driver-closed') -and ($d.closure_reason -eq $case.name) -and ($d.closure_record -eq $record)) "DpmMode 0 with $record $($case.code): $($d.decision) ($($d.closure_reason))"
        $d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; $record = $case.code } -Reopen) 'DpmMode'
        Check (($d.decision -eq 'reopened') -and ($d.value -eq 1)) "DpmMode 0 with $record $($case.code), repair: $($d.decision) -> $($d.value)"
    }
}
# A legacy record holds the reason of any start, so only the three reasons above are a closure there.
foreach ($reason in 0, 1, 2, 5, 6, 7) {
    $d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; DpmLastReason = $reason } -Reopen) 'DpmMode'
    Check ($d.decision -eq 'kept') "DpmMode 0 with DpmLastReason $reason (not a guard fallback): $($d.decision)"
}
# Only PersistFallback writes a durable record, so any reason in it is the driver's act: a later caller with a new
# reason must not read as a setting of the tester. An unnamed code is reported by its number.
foreach ($reason in 1, 2, 5, 6, 7) {
    $d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; DpmClosedReason = $reason; DpmLastReason = 1 }) 'DpmMode'
    Check (($d.decision -eq 'driver-closed') -and ($d.closure_record -eq 'DpmClosedReason') -and ($d.closure_reason -eq "reason $reason") -and ($d.closure -eq 'the driver went back to the base clock itself')) "DpmMode 0 with DpmClosedReason $reason (a reason the table does not name): $($d.decision) ($($d.closure_reason))"
    $d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; DpmClosedReason = $reason; DpmLastReason = 1 } -Reopen) 'DpmMode'
    Check (($d.decision -eq 'reopened') -and ($d.value -eq 1) -and (@($d.clear) -contains 'DpmClosedReason')) "DpmMode 0 with DpmClosedReason $reason, repair: $($d.decision) -> $($d.value)"
}
# The durable record decides where both exist: a 0.7.208.1 fallback whose boot has passed holds 4 in one and 1 in
# the other, and the reading must not fall back to the start's own reason.
$d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; DpmClosedReason = 8; DpmLastReason = 1 }) 'DpmMode'
Check (($d.decision -eq 'driver-closed') -and ($d.closure_record -eq 'DpmClosedReason') -and ($d.closure_reason -eq 'smu-error')) "both records: the durable one decides ($($d.closure_record) $($d.closure_reason))"
# A durable record with nothing in it does not hide the legacy one: the legacy record is read instead.
$d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; DpmClosedReason = 0; DpmLastReason = 4 }) 'DpmMode'
Check (($d.decision -eq 'driver-closed') -and ($d.closure_record -eq 'DpmLastReason') -and ($d.closure_reason -eq 'unclean')) "an empty durable record, DpmLastReason 4: read through $($d.closure_record)"
$d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMode = 0; DpmClosedReason = 0; DpmLastReason = 1 }) 'DpmMode'
Check ($d.decision -eq 'kept') "an empty durable record, DpmLastReason 1: $($d.decision)"
# The record is deleted with the write, and the restore set does not write it back.
$key = 'HKCU:\Software\amdgpu-wddm-installer-test'
if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
try {
    $k = "$key\Closure"
    Initialize-RegistryKey $k
    foreach ($e in $afterClose.GetEnumerator()) { New-ItemProperty -LiteralPath $k -Name $e.Key -Value ([int]$e.Value) -PropertyType DWord -Force | Out-Null }
    $before = Read-RegistryValues $k
    $plan = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $before -Owned ([ordered]@{ UnconfirmedStarts = 0 }) -After (Read-RegistryValues $k) -Restore @($before.Keys) -Reopen
    Write-RegistryPlan $k $plan
    $v = Read-RegistryValues $k
    Check (($v.EnableGpuPresentBlit -eq 1) -and ($v.EnableCddDwmInterop -eq 1) -and ($v.DpmMode -eq 1)) "after the repair write: EnableGpuPresentBlit $($v.EnableGpuPresentBlit), EnableCddDwmInterop $($v.EnableCddDwmInterop), DpmMode $($v.DpmMode)"
    Check (-not $v.ContainsKey('InteropClosedReason')) 'InteropClosedReason is gone, the way the driver reads it at the next start'
    Check (-not $v.ContainsKey('DpmClosedReason')) 'DpmClosedReason is gone too: the tester asked for the automatic clock again'
    Check ($v.DpmLastReason -eq 4) 'DpmLastReason stays: it is the record of the last start, which the next start overwrites'
    $again = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current (Read-RegistryValues $k) -Reopen
    Check (@($again | Where-Object { $_.decision -in 'reopened', 'driver-closed' }).Count -eq 0) 'a second repair over the result finds no closure left'
    # The order inside Write-RegistryPlan: the value first, the record after it. A write that throws must leave the
    # record where it is, so that the next install still reads the closure (BD-069). The probe is a hand-made entry
    # that writes a name and clears the same name: with the write first the name is gone afterwards.
    $probe = "$key\Order"
    Write-RegistryPlan $probe @([pscustomobject]@{ name = 'DpmMode'; value = 1; write = $true; clear = @('DpmMode') })
    Check (-not (Read-RegistryValues $probe).ContainsKey('DpmMode')) 'Write-RegistryPlan writes the value before it clears the record'
} finally { Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue }
Check (-not (Test-Path -LiteralPath $key)) 'closure scratch key removed'

'allowlist'
$plan = Get-RegistryDefaultPlan -Defaults $next.app_router -Previous $legacy.app_router -Current @{ Mode = 'allowlist'; Allow = [string[]]@('dxdiag.exe', 'game.exe'); Deny = [string[]]@('witcher3.exe') }
Check ((Get-Decision $plan 'Allow').decision -eq 'kept') 'Allow with the tester''s game.exe kept'
$d = Get-Decision $plan 'Deny'
Check (($d.decision -eq 'update') -and (@($d.value).Count -eq 2)) "Deny as tester.7 wrote it -> the new list: $($d.decision)"
$d = Get-Decision $plan 'Mode'
Check (($d.decision -eq 'update') -and ($d.value -eq 'gpu-default') -and $d.write) "Mode allowlist as tester.1 to tester.11 wrote it -> gpu-default: $($d.decision)"
$plan = Get-RegistryDefaultPlan -Defaults $next.app_router -Previous $legacy.app_router -Current @{ Mode = 'cpu' }
Check ((Get-Decision $plan 'Mode').decision -eq 'kept') 'Mode cpu (the tester''s kill switch) kept'
$plan = Get-RegistryDefaultPlan -Defaults $legacy.app_router -Previous $legacy.app_router -Current @{ Allow = [string[]]@('DXDIAG.EXE') }
Check ((Get-Decision $plan 'Allow').decision -eq 'same') 'Allow compares without case'

'command line and installer-owned values'
$plan = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ DpmMaxMHz = 1200; UnconfirmedStarts = 2 } -Explicit @{ DpmMaxMHz = 1800; CuMode = 40 } -Owned ([ordered]@{ UnconfirmedStarts = 0 })
$d = Get-Decision $plan 'DpmMaxMHz'
Check (($d.decision -eq 'command') -and ($d.value -eq 1800) -and $d.write) '-DpmMaxMHz 1800 over the tester''s 1200'
$d = Get-Decision $plan 'CuMode'
Check (($d.decision -eq 'command') -and ($d.value -eq 40) -and $d.write) '-CuMode 40 (not in the table) written'
$d = Get-Decision $plan 'UnconfirmedStarts'
Check (($d.decision -eq 'installer') -and ($d.value -eq 0) -and $d.write) 'UnconfirmedStarts reset to 0 (installer-owned)'
$text = Format-RegistryPlan $plan
Check ($text -match 'DpmMaxMHz=1800 \(command line\)') "plan text: $($text.Substring(0, [Math]::Min(120, $text.Length)))..."

'record: Release\AppliedDefaults round trip'
$json = $old | ConvertTo-Json -Depth 6 -Compress
$back = $json | ConvertFrom-Json
Check ((Test-RegistryValueSame $back.app_router.Allow $old.app_router.Allow) -and ($back.app_router.Allow -is [array])) "a one-entry list stays a list ($($json.Length) characters)"
$plan = Get-RegistryDefaultPlan -Defaults $next.desktop_router -Previous $back.desktop_router -Current @{ DwmForceCpu = 1 }
Check ((Get-Decision $plan 'DwmForceCpu').decision -eq 'update') 'the record drives the update like legacy_applied'

'this package''s table over tester.6/.7 (no record)'
$ship = [int]$table.defaults.desktop_router.DwmForceCpu
$d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.desktop_router -Previous $legacy.desktop_router -Current @{ DwmForceCpu = 1 }) 'DwmForceCpu'
Check (($d.value -eq $ship) -and ($d.decision -eq $(if ($ship -eq 1) { 'same' } else { 'update' }))) "DwmForceCpu 1 written by tester.7 -> $ship ($($d.decision))"
if ($ship -eq 0) {
    $d = Get-Decision (Get-RegistryDefaultPlan -Defaults $table.defaults.desktop_router -Previous $old.desktop_router -Current @{ DwmForceCpu = 1 }) 'DwmForceCpu'
    Check ($d.decision -eq 'update') "DwmForceCpu 1 recorded by an earlier release with default 1 -> 0 ($($d.decision))"
}

'write and read back (HKCU scratch key)'
$key = 'HKCU:\Software\amdgpu-wddm-installer-test'
if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
try {
    $sub = "$key\AppRouter"
    $plan = Get-RegistryDefaultPlan -Defaults $table.defaults.app_router -Previous $legacy.app_router -Current (Read-RegistryValues $sub) -Owned ([ordered]@{ GpuUmdPath = 'C:\x\d3d11.dll' })
    Write-RegistryPlan $sub $plan
    $k = Get-Item -LiteralPath $sub
    Check ($k.GetValueKind('Allow') -eq 'MultiString') 'Allow is REG_MULTI_SZ'
    Check ($k.GetValueKind('Mode') -eq 'String') 'Mode is REG_SZ'
    $sub2 = "$key\Parameters"
    Write-RegistryPlan $sub2 (Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{})
    Check ((Get-Item -LiteralPath $sub2).GetValueKind('DpmMaxMHz') -eq 'DWord') 'DpmMaxMHz is REG_DWORD'
    $again = Get-RegistryDefaultPlan -Defaults $table.defaults.app_router -Previous $legacy.app_router -Current (Read-RegistryValues $sub)
    Check (@($again | Where-Object { $_.decision -ne 'same' }).Count -eq 0) 'a second run finds every value unchanged'
    $again = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current (Read-RegistryValues $sub2)
    Check (@($again | Where-Object { $_.decision -ne 'same' }).Count -eq 0) 'parameters: second run unchanged'
    New-ItemProperty -LiteralPath $sub -Name Allow -Value ([string[]]@('dxdiag.exe', 'game.exe')) -PropertyType MultiString -Force | Out-Null
    # The release before the upgrade is this table, which wrote the values above (empty Allow and Deny since b20).
    $p3 = Get-RegistryDefaultPlan -Defaults $next.app_router -Previous $table.defaults.app_router -Current (Read-RegistryValues $sub)
    Write-RegistryPlan $sub $p3
    $v = (Get-Item -LiteralPath $sub).GetValue('Allow')
    Check (($v -join ',') -eq 'dxdiag.exe,game.exe') "upgrade keeps the tester's Allow: $($v -join ',')"
    Check (((Get-Item -LiteralPath $sub).GetValue('Deny') -join ',') -eq 'witcher3.exe,other.exe') 'upgrade writes the new Deny'
} finally { Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue }
Check (-not (Test-Path -LiteralPath $key)) 'scratch key removed'

'the driver package resets the gates between the judgement and the write (pnputil runs the INF AddReg)'
$inf = Join-Path (Split-Path $Installer) 'payload\kmd\bc250kmd.inf'
if (Test-Path -LiteralPath $inf) { $infNames = Get-InfParameterNames $inf; $infSource = 'the package INF' }
else { $infNames = @('UnconfirmedStarts', 'EnableMmio', 'EnableMmioWrite', 'EnableVram', 'EnableVramWrite', 'EnableGart', 'EnablePsp', 'EnableGfx', 'EnableIh', 'EnableDcnWrite', 'EnableVidPnFlip', 'EnableFullWddm', 'EnableGpuVa', 'EnableGpuSubmit', 'EnablePagingNode', 'EnablePresentBlit', 'EnableHangBugcheck', 'KeepLog'); $infSource = 'the 0.7.199.1 INF list, the same names as 0.7.198.2 (no package INF next to this installer)' }
Check ((@($infNames) -contains 'EnableFullWddm') -and (@($infNames) -contains 'EnableMmioWrite')) "$(@($infNames).Count) INF Parameters names from $infSource"
$key = 'HKCU:\Software\amdgpu-wddm-installer-test'
function Invoke-InfReset([string]$K) { foreach ($n in $infNames) { New-ItemProperty -LiteralPath $K -Name $n -Value 0 -PropertyType DWord -Force | Out-Null } }
function Get-Snapshot([string]$K) { $all = Read-RegistryValues $K; $h = @{}; foreach ($n in @($infNames) + @(ConvertTo-PairList $table.defaults.parameters | ForEach-Object { $_.Name })) { if ($all.ContainsKey($n)) { $h[$n] = $all[$n] } }; return $h }
if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
try {
    # Upgrade over tester.6: its defaults, a tester's DpmMaxMHz and interop switch, the INF-only values set by hand,
    # and a value of the KMD's own that nobody judges.
    $k = "$key\Upgrade"
    Write-RegistryPlan $k (Get-RegistryDefaultPlan -Defaults $legacy.parameters -Previous $null -Current @{})
    foreach ($e in @{ DpmMaxMHz = 1200; EnableCddDwmInterop = 0; EnableMmioWrite = 1; EnableHangBugcheck = 1; DpmLastMode = 1 }.GetEnumerator()) { New-ItemProperty -LiteralPath $k -Name $e.Key -Value $e.Value -PropertyType DWord -Force | Out-Null }
    $before = Get-Snapshot $k
    Invoke-InfReset $k
    $after = Read-RegistryValues $k
    $wrong = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $after
    Check ((Get-Decision $wrong 'EnableFullWddm').decision -eq 'kept') 'judged after the reset, EnableFullWddm 0 would be KEPT (the tester.10 candidate defect)'
    $plan = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $before -Owned ([ordered]@{ UnconfirmedStarts = 0 }) -After $after -Restore $infNames
    Write-RegistryPlan $k $plan
    $v = Read-RegistryValues $k
    Check ($v.EnableFullWddm -eq 2) "EnableFullWddm $($v.EnableFullWddm) after the write (judged before the reset: $((Get-Decision $plan 'EnableFullWddm').decision), rewritten)"
    $gates = @(ConvertTo-PairList $table.defaults.parameters | Where-Object { $_.Name -like 'Enable*' -and $_.Name -ne 'EnableCddDwmInterop' } | Where-Object { -not (Test-RegistryValueSame $v[$_.Name] $_.Value) } | ForEach-Object { $_.Name })
    Check ($gates.Count -eq 0) "every table gate back at its default$(if ($gates.Count) { ': not ' + ($gates -join ', ') })"
    Check (($v.DpmMaxMHz -eq 1200) -and ($v.EnableCddDwmInterop -eq 0)) "tester values kept over the reset: DpmMaxMHz $($v.DpmMaxMHz), EnableCddDwmInterop $($v.EnableCddDwmInterop)"
    Check (($v.EnableMmioWrite -eq 1) -and ($v.EnableHangBugcheck -eq 1)) "INF-only values as before the install: EnableMmioWrite $($v.EnableMmioWrite), EnableHangBugcheck $($v.EnableHangBugcheck)"
    Check (($v.UnconfirmedStarts -eq 0) -and ($v.DpmLastMode -eq 1)) 'UnconfirmedStarts 0 (installer), DpmLastMode untouched'
    Check ((Format-RegistryPlan $plan) -match 'EnableFullWddm=2 \(unchanged\) \[written again over the driver package reset\]') 'the plan text says the value is written again'
    $again = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $before -After (Read-RegistryValues $k) -Restore $infNames
    Check (@($again | Where-Object { $_.write }).Count -eq 0) 'a re-run with the same snapshot writes nothing more'
    # Fresh install: nothing before; the INF's values for names outside the table stay.
    $k = "$key\Fresh"
    Initialize-RegistryKey $k
    $before = Get-Snapshot $k
    Invoke-InfReset $k
    $plan = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $before -Owned ([ordered]@{ UnconfirmedStarts = 0 }) -After (Read-RegistryValues $k) -Restore $infNames
    Write-RegistryPlan $k $plan
    $v = Read-RegistryValues $k
    Check (($v.EnableFullWddm -eq 2) -and ($v.EnableMmio -eq 1) -and ($v.DpmMaxMHz -eq 1500) -and ($v.EnableMmioWrite -eq 0)) "fresh install: EnableFullWddm $($v.EnableFullWddm), EnableMmio $($v.EnableMmio), DpmMaxMHz $($v.DpmMaxMHz), EnableMmioWrite $($v.EnableMmioWrite) (INF)"

    # The whole key, as install.ps1 takes it now: a value that this release's table and the INF do not name goes back
    # as it was. Unit A lost CuMode 40 at every release install before this one, because the snapshot kept the judged
    # names alone; the GPU then ran on 24 of its 40 compute units with nothing saying so.
    $k = "$key\WholeKey"
    Write-RegistryPlan $k (Get-RegistryDefaultPlan -Defaults $legacy.parameters -Previous $null -Current @{})
    foreach ($e in @{ CuMode = 40; CuModeConfirmed = 1; DpmMaxMHz = 1200 }.GetEnumerator()) { New-ItemProperty -LiteralPath $k -Name $e.Key -Value $e.Value -PropertyType DWord -Force | Out-Null }
    New-ItemProperty -LiteralPath $k -Name 'LabNote' -Value 'unit A' -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name 'SomeBlob' -Value ([byte[]](1, 2, 3, 4)) -PropertyType Binary -Force | Out-Null
    $judged = @(@($infNames) + @(ConvertTo-PairList $table.defaults.parameters | ForEach-Object { $_.Name })) | Select-Object -Unique
    $whole = Read-RegistryValues $k
    Check ((Test-RestorableRegistryValue $whole['CuMode']) -and (Test-RestorableRegistryValue $whole['LabNote']) -and -not (Test-RestorableRegistryValue $whole['SomeBlob'])) 'a REG_DWORD and a REG_SZ value go back through the plan, a REG_BINARY value does not'
    $restore = @(@($infNames) + @($whole.Keys | Where-Object { $_ -notin $judged -and (Test-RestorableRegistryValue $whole[$_]) })) | Select-Object -Unique
    # What the driver package install does to the service key: the other values are gone, the INF's names are 0.
    Remove-Item -LiteralPath $k -Recurse -Force
    Initialize-RegistryKey $k
    Invoke-InfReset $k
    $plan = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $whole -Owned ([ordered]@{ UnconfirmedStarts = 0 }) -After (Read-RegistryValues $k) -Restore $restore
    Write-RegistryPlan $k $plan
    $v = Read-RegistryValues $k
    Check (((Get-Decision $plan 'CuMode').decision -eq 'restored') -and ($v.CuMode -eq 40) -and ($v.CuModeConfirmed -eq 1)) "CuMode $($v.CuMode) and CuModeConfirmed $($v.CuModeConfirmed) back as before the driver package"
    Check ($v.LabNote -eq 'unit A') "a REG_SZ value outside the table back as before: LabNote '$($v.LabNote)'"
    Check (-not $v.ContainsKey('SomeBlob')) 'a REG_BINARY value is not written back (the installer names it in its log)'
    Check ($v.DpmMaxMHz -eq 1200) "a value of the table that the tester changed is still kept: DpmMaxMHz $($v.DpmMaxMHz)"
    $again = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current $whole -After (Read-RegistryValues $k) -Restore $restore
    Check (@($again | Where-Object { $_.write }).Count -eq 0) 'the whole-key plan writes nothing more on a re-run with the same snapshot'
    # The command line over a value on the computer: one entry for the name, and the command line wins. Two entries
    # would write the value from before the install over the value the tester asked for.
    $cmd = Get-RegistryDefaultPlan -Defaults $table.defaults.parameters -Previous $legacy.parameters -Current @{ CuMode = 24 } -Explicit @{ CuMode = 40 } -After @{ CuMode = 24 } -Restore @('CuMode')
    Check ((@($cmd | Where-Object { $_.name -eq 'CuMode' }).Count -eq 1) -and ((Get-Decision $cmd 'CuMode').decision -eq 'command') -and ((Get-Decision $cmd 'CuMode').value -eq 40)) '-CuMode 40 is judged once, and the command line wins over the value from before the install'
} finally { Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue }
Check (-not (Test-Path -LiteralPath $key)) 'scratch key removed'

if ($fail) { "FAILED: $fail check(s)"; exit 1 }
'registry defaults: all checks passed'
exit 0
