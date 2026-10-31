# Host test of BD-191: the legacy top-level copies under Services\bc250kmd\Parameters.
# The field case: after the tester.30 -> tester.31 upgrade the lab's 22631 kept 57 top-level values and the
# StartRecord and HangRecovery subkeys under Parameters. No driver of the shared state reads them, but the
# installer's start-failure diagnostics read StartRecord from there, and the installer wrote the start-screen answer
# (BootLogo) there, where the driver no longer looks.
#
# Contracts:
#   1. The plan (Get-LegacyParametersPlan over the real driver/state/catalog.json): a top-level value whose name the
#      catalog owns at the root path and a subkey whose name begins a catalog path are legacy; DefaultsV2 and every
#      name outside the catalog stay. The raw selectors that the KMD reads from Parameters (guard.c) are outside the
#      catalog, so the plan can never take one of them.
#   2. The archive (Invoke-LegacyParametersArchive on a scratch key under HKCU): the archive file holds every legacy
#      copy with its exact kind and data before anything is deleted; the copies are then gone and the rest is
#      unchanged; a second run finds nothing; a failed archive write and an empty snapshot delete nothing.
#   3. The start-screen answer: Get-BootLogoPlan carries an earlier top-level BootLogo 0 into the state once, and
#      Set-BootLogoChoice writes through the state client only (a memory-only client here).
#   4. install.ps1: the diagnostics read the shared state, the cleanup runs after the acknowledged state
#      transaction and after the start-screen step, and never on a legacy rollback.
# Windows PowerShell 5.1, like the installer:
#   powershell -NoProfile -File tools\release\test-legacy-parameters.ps1 [-WorkRoot <dir>]
# Writes only under -WorkRoot and under HKCU\Software\amdgpu-wddm-test\bd191-<id>, which it removes again.
param([string]$Installer = (Join-Path $PSScriptRoot 'installer'), [string]$WorkRoot = (Join-Path $env:TEMP 'amdgpu-wddm-bd191'))
$ErrorActionPreference = 'Stop'
. (Join-Path $Installer 'common.ps1')
. (Join-Path $Installer 'state-reader.ps1')
$repo = Split-Path (Split-Path $PSScriptRoot)
$fail = 0
function Check([bool]$Ok, [string]$Text) { if ($Ok) { "  PASS $Text" } else { "  FAIL $Text"; $script:fail++ } }
$work = Join-Path $WorkRoot ('bd191-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
[void][IO.Directory]::CreateDirectory($work)
$script:LogPath = Join-Path $work 'install.log'
$script:DryRunMode = $false

$catalog = Get-Content -LiteralPath (Join-Path $repo 'driver\state\catalog.json') -Raw | ConvertFrom-Json
$snapshot = [pscustomobject]@{ abi = 1; error = 0; items = @($catalog.values | ForEach-Object { [pscustomobject]@{ path = $_.path; name = $_.name; kind = $_.kind } }) }
$names = Get-StateCatalogParameterNames -Snapshot $snapshot -DefaultsChild $catalog.defaults_child
$rootNames = @($catalog.values | Where-Object { $_.path -eq '' -and $_.kind -eq 'setting' } | ForEach-Object { [string]$_.name })

'1. the plan over the real catalog'
Check ($catalog.defaults_child -eq 'DefaultsV2') "the catalog's defaults child is DefaultsV2 ($($catalog.defaults_child))"
Check ($names.values.Contains('CuMode') -and $names.values.Contains('DpmMaxMHz') -and $names.values.Contains('BootLogo')) 'CuMode, DpmMaxMHz and BootLogo are catalog names at the root path'
Check ($names.subkeys.Contains('StartRecord') -and $names.subkeys.Contains('StopRecord') -and $names.subkeys.Contains('HangRecovery')) 'StartRecord, StopRecord and HangRecovery begin catalog paths'
Check (-not $names.subkeys.Contains('DefaultsV2')) 'DefaultsV2 is never a legacy subkey'
$guard = Get-Content -LiteralPath (Join-Path $repo 'driver\kmd\guard.c') -Raw
$raw = @([regex]::Matches($guard, '(?:ParameterDword|LabQueryQword)\(key, L"(\w+)"') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
Check ($raw.Count -ge 5) "guard.c reads raw selectors from Parameters: $($raw -join ', ')"
$overlap = @($raw | Where-Object { $names.values.Contains($_) })
Check ($overlap.Count -eq 0) "no raw selector of guard.c is a catalog name, so the cleanup cannot take one ($(if ($overlap.Count) { $overlap -join ', ' } else { 'none' }))"
$plan = Get-LegacyParametersPlan -ValueNames @('CuMode', 'dpmmaxmhz', 'BootLogo', 'EnableHardwarePointer', 'LabScaledPlane', 'PagingRingStartBytes', 'NotInCatalog', '') `
    -SubKeyNames @('DefaultsV2', 'StartRecord', 'hangrecovery', 'StopRecord', 'Other') -Names $names
Check ((@($plan.remove_values) -join ',') -ceq 'CuMode,dpmmaxmhz,BootLogo') "legacy values: catalog names, any case ($(@($plan.remove_values) -join ','))"
Check ((@($plan.kept_values) -join ',') -ceq 'EnableHardwarePointer,LabScaledPlane,PagingRingStartBytes,NotInCatalog,') "kept values: raw selectors, unknown names and the default value ($(@($plan.kept_values) -join ','))"
Check ((@($plan.remove_subkeys) -join ',') -ceq 'StartRecord,hangrecovery,StopRecord') "legacy subkeys ($(@($plan.remove_subkeys) -join ','))"
Check ((@($plan.kept_subkeys) -join ',') -ceq 'DefaultsV2,Other') "kept subkeys ($(@($plan.kept_subkeys) -join ','))"

'2. the archive on a scratch key'
$scratch = 'Software\amdgpu-wddm-test\bd191-' + [guid]::NewGuid().ToString('N')
$params = "$scratch\Parameters"
$hkcu = [Microsoft.Win32.Registry]::CurrentUser
$bytes = [byte[]](0, 1, 2, 250, 255)
function New-Fixture {
    $k = $hkcu.CreateSubKey($params)
    try {
        $k.SetValue($rootNames[0], -1, 'DWord')
        $k.SetValue($rootNames[1], [int64]-1, 'QWord')
        $k.SetValue($rootNames[2], $bytes, 'Binary')
        $k.SetValue($rootNames[3], [string[]]@('one', 'two'), 'MultiString')
        $k.SetValue($rootNames[4], '%SystemRoot%\x', 'ExpandString')
        $k.SetValue('EnableHardwarePointer', 1, 'DWord')
        $k.SetValue('NotInCatalog', 'kept', 'String')
        $d = $k.CreateSubKey('DefaultsV2'); $d.SetValue('CatalogId', 'c8fe'); $d.Dispose()
        $s = $k.CreateSubKey('StartRecord'); $s.SetValue('Result', 'started'); $s.SetValue('LogLines', [string[]]@('a', 'b'), 'MultiString'); $s.Dispose()
        $w = $k.CreateSubKey('HangRecovery\Waves'); $w.SetValue('Snapshot', $bytes, 'Binary'); $w.Dispose()
        $o = $k.CreateSubKey('Other'); $o.SetValue('X', 7, 'DWord'); $o.Dispose()
    } finally { $k.Dispose() }
}
try {
    New-Fixture
    $archiveDir = Join-Path $work 'archive'
    $r = Invoke-LegacyParametersArchive -Snapshot $snapshot -ArchiveDirectory $archiveDir -Root $hkcu -SubKey $params -PackageVersion '0.0.0-test'
    Check ($r.outcome -eq 'archived') "the run archives ($($r.outcome))"
    Check ((@($r.values).Count -eq 5) -and (@($r.subkeys).Count -eq 2) -and (@($r.problems).Count -eq 0)) "5 values and 2 subkeys deleted, no problem ($(@($r.values).Count), $(@($r.subkeys).Count), $(@($r.problems) -join '; '))"
    $files = @(Get-ChildItem -LiteralPath $archiveDir -File)
    Check (($files.Count -eq 1) -and ($files[0].Name -like 'legacy-parameters-*.json')) "one archive file, no temporary file ($(@($files | ForEach-Object Name) -join ', '))"
    $a = Get-Content -LiteralPath $r.path -Raw | ConvertFrom-Json
    $av = @{}; foreach ($v in @($a.values)) { $av[$v.name] = $v }
    Check (($av[$rootNames[0]].kind -eq 'DWord') -and ([uint64]$av[$rootNames[0]].data -eq 4294967295)) "a DWORD with the high bit keeps its value ($($av[$rootNames[0]].data))"
    Check (($av[$rootNames[1]].kind -eq 'QWord') -and ($av[$rootNames[1]].data -eq '18446744073709551615')) "a QWORD keeps all 64 bits as text ($($av[$rootNames[1]].data))"
    Check (($av[$rootNames[2]].kind -eq 'Binary') -and ($av[$rootNames[2]].data -eq [Convert]::ToBase64String($bytes))) 'binary data as base64, exact'
    Check (($av[$rootNames[3]].kind -eq 'MultiString') -and ((@($av[$rootNames[3]].data) -join '|') -eq 'one|two')) 'a multi-string keeps its strings in order'
    Check (($av[$rootNames[4]].kind -eq 'ExpandString') -and ($av[$rootNames[4]].data -eq '%SystemRoot%\x')) 'an expandable string is not expanded'
    $hr = @($a.subkeys | Where-Object { $_.name -eq 'HangRecovery' })[0]
    Check ($hr -and ($hr.subkeys[0].name -eq 'Waves') -and ($hr.subkeys[0].values[0].data -eq [Convert]::ToBase64String($bytes))) 'a nested legacy subkey is archived with its children'
    Check (($a.schema -eq 1) -and ($a.package_version -eq '0.0.0-test') -and ($a.key -like 'HKEY_CURRENT_USER\*\Parameters')) "the archive names its schema, the package and the key ($($a.key))"
    $k = $hkcu.OpenSubKey($params)
    try {
        $left = @($k.GetValueNames() | Sort-Object); $leftKeys = @($k.GetSubKeyNames() | Sort-Object)
        Check ((@($left) -join ',') -ceq 'EnableHardwarePointer,NotInCatalog') "only the names outside the catalog stay ($($left -join ','))"
        Check ((@($leftKeys) -join ',') -ceq 'DefaultsV2,Other') "DefaultsV2 and the unknown subkey stay ($($leftKeys -join ','))"
        $d = $k.OpenSubKey('DefaultsV2'); Check ($d.GetValue('CatalogId') -eq 'c8fe') 'DefaultsV2 is unchanged'; $d.Dispose()
        Check ($k.GetValue('EnableHardwarePointer') -eq 1) 'the raw selector keeps its value'
    } finally { $k.Dispose() }
    $r2 = Invoke-LegacyParametersArchive -Snapshot $snapshot -ArchiveDirectory $archiveDir -Root $hkcu -SubKey $params
    Check (($r2.outcome -eq 'none') -and (@(Get-ChildItem -LiteralPath $archiveDir -File).Count -eq 1)) "a second run finds nothing and writes no file ($($r2.outcome))"

    New-Fixture
    $notDir = Join-Path $work 'not-a-directory'
    Set-Content -LiteralPath $notDir -Value 'x'
    $threw = $null
    try { [void](Invoke-LegacyParametersArchive -Snapshot $snapshot -ArchiveDirectory $notDir -Root $hkcu -SubKey $params) } catch { $threw = $_.Exception.Message }
    $k = $hkcu.OpenSubKey($params)
    try { $still = ($null -ne $k.GetValue($rootNames[0])) -and ($k.GetSubKeyNames() -contains 'StartRecord') } finally { $k.Dispose() }
    Check ($threw -and $still) "an archive that cannot be written stops the run and deletes nothing ($threw)"
    $threw = $null
    try { [void](Invoke-LegacyParametersArchive -Snapshot ([pscustomobject]@{ items = @() }) -ArchiveDirectory $archiveDir -Root $hkcu -SubKey $params) } catch { $threw = $_.Exception.Message }
    $k = $hkcu.OpenSubKey($params)
    try { $still = ($null -ne $k.GetValue($rootNames[0])) } finally { $k.Dispose() }
    Check ($threw -and $still) "an empty snapshot stops the run and deletes nothing ($threw)"
    $r3 = Invoke-LegacyParametersArchive -Snapshot $snapshot -ArchiveDirectory $archiveDir -Root $hkcu -SubKey "$scratch\NoSuchKey"
    Check ($r3.outcome -eq 'absent') "no Parameters key: absent ($($r3.outcome))"
} finally { $hkcu.DeleteSubKeyTree($scratch, $false) }
Check ($null -eq $hkcu.OpenSubKey($scratch)) 'the scratch key is removed'

'3. the start-screen answer'
$p = Get-BootLogoPlan -Stated $null -Current $null -Legacy 0
Check (($p.action -eq 'write') -and $p.carried -and (-not $p.selected)) 'an earlier top-level BootLogo 0 with no override goes into the state'
Check ((Get-BootLogoPlan -Stated $null -Current $null -Legacy 1).action -eq 'keep') 'a top-level 1 is the default and writes nothing'
Check ((Get-BootLogoPlan -Stated $true -Current $null -Legacy 0).action -eq 'none') 'a stated on wins over an earlier top-level 0'
$p = Get-BootLogoPlan -Stated $null -Current 1 -Legacy 0
Check (($p.action -eq 'keep') -and $p.selected) 'an override wins over the top-level copy'
Check ((Get-BootLogoChoice -Stated $null -Current $null -Legacy 0) -eq $false) 'the box of an upgrade opens on the earlier top-level answer'
$script:mem = @{}; $script:calls = @()
$runner = {
    param($exe, $arguments)
    $script:calls += ,(@($arguments) -join ' ')
    switch ($arguments[1]) {
        'snapshot' {
            $o = if ($script:mem.ContainsKey('BootLogo')) { @{ status = 'ok'; value = $script:mem['BootLogo'] } } else { @{ status = 'absent' } }
            return @{ code = 0; text = (@{ abi = 1; error = 0; items = @(@{ path = ''; name = 'BootLogo'; kind = 'setting'; override = $o; effective = @{ status = 'ok'; value = 1 } }) } | ConvertTo-Json -Depth 6 -Compress) }
        }
        'set' { $script:mem[$arguments[2]] = [uint32]$arguments[3]; return @{ code = 0; text = '{"abi":1,"error":0}' } }
        'clear' { [void]$script:mem.Remove($arguments[2]); return @{ code = 0; text = '{"abi":1,"error":0}' } }
        'get' { return @{ code = 0; text = (@{ abi = 1; error = 0; result = @{ status = 'ok'; type = 4; bytes = 4; value = $script:mem[$arguments[2]] } } | ConvertTo-Json -Depth 6 -Compress) } }
    }
}
[void](Set-BootLogoChoice -Stated $false -Cli 'fixture.exe' -Runner $runner -ReadLegacy { $null })
Check ($script:mem['BootLogo'] -eq 0) "-NoBootLogo sets the override 0 ($(@($script:calls) -join '; '))"
$script:calls = @()
[void](Set-BootLogoChoice -Stated $true -Cli 'fixture.exe' -Runner $runner -ReadLegacy { $null })
Check ((-not $script:mem.ContainsKey('BootLogo')) -and (@($script:calls) -contains 'state clear BootLogo')) "-BootLogo clears the override ($(@($script:calls) -join '; '))"
[void](Set-BootLogoChoice -Stated $null -Cli 'fixture.exe' -Runner $runner -ReadLegacy { 0 })
Check ($script:mem['BootLogo'] -eq 0) 'an earlier top-level 0 is carried into the override'
$script:calls = @()
[void](Set-BootLogoChoice -Stated $null -Cli 'fixture.exe' -Runner $runner -ReadLegacy { 0 })
Check (@($script:calls | Where-Object { $_ -like 'state set*' }).Count -eq 0) 'once carried, a later run writes nothing'
$failing = { param($exe, $arguments) $script:calls += ,(@($arguments) -join ' '); @{ code = 5; text = 'denied' } }
$script:calls = @()
$u = Set-BootLogoChoice -Stated $false -Cli 'fixture.exe' -Runner $failing -ReadLegacy { $null }
Check (($u.action -eq 'unread') -and (@($script:calls | Where-Object { $_ -like 'state set*' }).Count -eq 0)) "a state that cannot be read changes nothing ($($u.action))"
$common = Get-Content -LiteralPath (Join-Path $Installer 'common.ps1') -Raw
Check ($common -notmatch 'New-ItemProperty -LiteralPath \$script:ParametersKey -Name BootLogo') 'the installer no longer writes the top-level BootLogo'

'4. install.ps1'
$install = Get-Content -LiteralPath (Join-Path $Installer 'install.ps1') -Raw
$call = [regex]::Match($install, '(?s)\$r = Write-StartFailureDiagnostics.{0,400}?-Context \$Context').Value
Check ($call -and ($call -notmatch 'ParametersSubKey')) 'the diagnostics read the start and stop records from the shared state, not from Parameters'
$iComplete = $install.IndexOf('Complete-DriverStateTransaction -State')
$iLogo = $install.IndexOf('[void](Set-BootLogoChoice -Stated $bootLogoStated -Cli')
$iArchive = $install.IndexOf('Invoke-LegacyParametersArchive -Snapshot')
Check (($iComplete -gt 0) -and ($iLogo -gt $iComplete) -and ($iArchive -gt $iLogo)) "the cleanup follows the state transaction and the start-screen step ($iComplete < $iLogo < $iArchive)"
Check ($install -match "if \(\(-not \`$legacyRollback\) -and \(-not \`$script:DryRunMode\) -and \`$state\.driver_state -and \`$state\.driver_state\.phase -eq 'acknowledged'\)") 'the cleanup runs only after an acknowledged transaction, never on a legacy rollback or a dry run'

if ($fail) { "$fail check(s) failed"; exit 1 }
'all checks passed'
