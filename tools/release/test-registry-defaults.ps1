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

'allowlist'
$plan = Get-RegistryDefaultPlan -Defaults $next.app_router -Previous $legacy.app_router -Current @{ Mode = 'allowlist'; Allow = [string[]]@('dxdiag.exe', 'game.exe'); Deny = [string[]]@('witcher3.exe') }
Check ((Get-Decision $plan 'Allow').decision -eq 'kept') 'Allow with the tester''s game.exe kept'
$d = Get-Decision $plan 'Deny'
Check (($d.decision -eq 'update') -and (@($d.value).Count -eq 2)) "Deny as tester.7 wrote it -> the new list: $($d.decision)"
$d = Get-Decision $plan 'Mode'
Check (($d.decision -eq 'update') -and ($d.value -eq 'gpu-default') -and $d.write) "Mode allowlist as tester.1 to tester.11 wrote it -> gpu-default: $($d.decision)"
$plan = Get-RegistryDefaultPlan -Defaults $next.app_router -Previous $legacy.app_router -Current @{ Mode = 'cpu' }
Check ((Get-Decision $plan 'Mode').decision -eq 'kept') 'Mode cpu (the tester''s kill switch) kept'
$plan = Get-RegistryDefaultPlan -Defaults $table.defaults.app_router -Previous $legacy.app_router -Current @{ Allow = [string[]]@('DXDIAG.EXE') }
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
    $p3 = Get-RegistryDefaultPlan -Defaults $next.app_router -Previous $legacy.app_router -Current (Read-RegistryValues $sub)
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
