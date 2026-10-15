# Host-only version/ranking regressions. No registry, package install or hardware access.
param()
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer\common.ps1')
$script:checks = 0
function Check([bool]$Ok, [string]$Why) {
    $script:checks++
    if (-not $Ok) { throw "FAIL: $Why" }
}
function Refuses([scriptblock]$Body, [string]$Why) {
    $refused = $false
    try { & $Body | Out-Null } catch { $refused = $true }
    Check $refused $Why
}
Check ((Get-PackageDriverQuad '0.7.216.103') -ceq '40.7.216.103') 'normal mapping'
Check ((Get-PackageDriverQuad '1.0.0.0') -ceq '41.0.0.0') 'major transition'
Check ((Get-PackageDriverQuad '65494.65534.65534.65534') -ceq '65534.65534.65534.65534') 'last admitted version'
foreach ($bad in @('', '1.2.3', '1.2.3.4.5', '01.2.3.4', '1.2.3.-4', '1.2.3.65535', '1.2.3.9999999999999',
        '1.2.3.4x', "1.2.3.4`n", ' 1.2.3.4', '1.2.3.4 ', '+1.2.3.4', '1,2,3,4')) {
    Refuses { ConvertTo-DriverQuad $bad } "quad refusal: $bad"
}
Refuses { Get-PackageDriverQuad '65495.0.0.0' } 'mapping overflow'
Refuses { Get-ProductPackageQuad '0.7.216.103-tester.32 extra' } 'release suffix junk'
Refuses { Get-ProductPackageQuad "0.7.216.103-tester.32`n" } 'release newline'
Check ((Get-ProductPackageQuad '0.7.216.103-tester.32v2') -ceq '0.7.216.103') 'revision suffix is not INF revision'
Refuses { ConvertTo-DriverDate '13/10/2026' } 'date independent of OS culture'
$release = '0.7.216.103-tester.32'
$hash = 'AB' * 32
$i = New-PackageVersionIdentity $release '10/13/2026' '0.7.216.64' '0x000700D8' $hash
$m = [pscustomobject]@{ version = $release; kmd_version = '40.7.216.103'; kmd_build = '0.7.216.64'; kmd_abi = '0x000700D8'; version_identity = $i }
$inf = "[Version]`r`nDriverVer = 10/13/2026,40.7.216.103`r`n[Bc250_PackageIdentity]`r`nHKR,, ProductPackageVersion, 0x00000000, `"0.7.216.103`"`r`n"
Check ((Get-ManifestDriverQuad $m) -ceq '40.7.216.103') 'three distinct package/product/KMD versions'
Check ((Test-PackageVersionIdentity $m $inf $hash).date -ceq '10/13/2026') 'complete package identity'
Check ((Get-ManifestDriverQuad ([pscustomobject]@{ kmd_version = '0.7.216.100' })) -ceq '0.7.216.100') 'legacy alias keeps its actual INF meaning'
Refuses { Test-PackageVersionIdentity $m ($inf.Replace('10/13/2026', '10/12/2026')) $hash } 'date mismatch'
Refuses { Test-PackageVersionIdentity $m ($inf.Replace('40.7.216.103', '40.7.216.64')) $hash } 'KMD build must not drive package numbering'
Refuses { Test-PackageVersionIdentity $m $inf ('CD' * 32) } 'wrong SYS'
Refuses { Test-PackageVersionIdentity $m ($inf.Replace('ProductPackageVersion', 'OtherValue')) $hash } 'no product HKR'
# The shipped INF writes the product quad as the %token% Windows resolves from [Strings], not as a literal.
$tokenInf = $inf.Replace('"0.7.216.103"', '%PackageProductVersion%') +
    "[Strings]`r`nPackageProductVersion = `"0.7.216.103`"`r`n"
Check ((Test-PackageVersionIdentity $m $tokenInf $hash).date -ceq '10/13/2026') 'product identity through the INF token'
Refuses { Test-PackageVersionIdentity $m ($tokenInf.Replace("[Strings]`r`nPackageProductVersion = `"0.7.216.103`"`r`n", '')) $hash } 'token with no string'
Refuses { Test-PackageVersionIdentity $m ($tokenInf.Replace('PackageProductVersion = "0.7.216.103"', 'PackageProductVersion = "0.7.216.64"')) $hash } 'token resolves to another quad'
Refuses { Get-InfDriverIdentity ($inf.Replace('[Bc250_PackageIdentity]', "DriverVer=10/13/2026,40.7.216.103`n[Bc250_PackageIdentity]")) } 'duplicate DriverVer'
$m.version_identity = $null
Refuses { Get-ManifestDriverQuad $m } 'new identity cannot fall back to legacy alias'
$m.version_identity = $i
$m.kmd_version = '0.7.216.103'
Refuses { Get-ManifestDriverQuad $m } 'inconsistent legacy alias refused'
$m.kmd_version = '40.7.216.103'
$i.kmd.build = '0.7.216.63'
Refuses { Get-ManifestDriverQuad $m } 'wrong embedded build'
$i.kmd.build = '0.7.216.64'
$store = @(
    [pscustomobject]@{ published = 'oem1.inf'; date = '10/10/2026'; version = '40.7.216.103' },
    [pscustomobject]@{ published = 'oem2.inf'; date = '10/11/2026'; version = '0.7.216.100' },
    [pscustomobject]@{ published = 'oem3.inf'; date = '10/13/2026'; version = '40.7.216.104' })
$plan = Get-DriverStoreRemovePlan $store 'oem3.inf' ''
Check ($plan.previous -ceq 'oem2.inf' -and $plan.remove.Count -eq 1 -and $plan.remove[0] -ceq 'oem1.inf') 'date wins before large mapped version'
$store[0].date = '10/11/2026'
$plan = Get-DriverStoreRemovePlan $store 'oem3.inf' ''
Check ($plan.previous -ceq 'oem1.inf') 'quad orders equal dates'
$store[0].date = $null
Check ((Get-DriverStoreRemovePlan $store 'oem3.inf' '').remove.Count -eq 0) 'unknown date forbids fallback deletion'
$store[0].date = '10/11/2026'; $store[0].version = '0.7.216.100'
Check ((Get-DriverStoreRemovePlan $store 'oem3.inf' '').remove.Count -eq 0) 'ambiguous order keeps both packages'
Check ((Get-DriverStoreRemovePlan $store 'oem3.inf' '0.7.216.100').keep.Count -eq 3) 'explicit prior version retains all matches'
$repo = Split-Path (Split-Path $PSScriptRoot)
$pnp = [IO.File]::ReadAllText((Join-Path $repo 'driver\kmd\pnp.c'))
Check (-not $pnp.Contains('DriverVersionStart(')) 'no runtime version rewrite at start'
Check (-not (Test-Path -LiteralPath (Join-Path $repo 'driver\kmd\driver_version.c'))) 'removed runtime alias walker'
# Run the real cleanup against stand-in OS readers and a private INF. Writes are
# collected, never sent to the registry. A refusal must leave every tag intact.
$savedWindows = $env:windir
$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('version-identity-' + [guid]::NewGuid().ToString('N'))
$script:deleted = @()
$script:boundService = $script:ServiceName
$script:boundInf = 'oem42.inf'
$script:boundKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\0001'
$script:propertyVersion = $m.kmd_version
$script:bindingValues = [pscustomobject]@{ DriverVersion = $m.kmd_version; ProductPackageVersion = $i.product_quad;
    Bc250DriverVersion = 'old'; Bc250ReportedDriverVersion = 'old' }
function Get-DeviceServiceName($InstanceId) { return $script:boundService }
function Get-DeviceDriverPackage($InstanceId) { return $script:boundInf }
function Get-DeviceDriverKey($InstanceId) { return $script:boundKey }
function Get-PnpDeviceProperty($InstanceId, $KeyName, $ErrorAction) { return [pscustomobject]@{ Data = $script:propertyVersion } }
function Get-ItemProperty($LiteralPath, $ErrorAction) { return $script:bindingValues }
function Remove-ItemProperty($LiteralPath, $Name, $ErrorAction) { $script:deleted += $Name }
function Invoke-Change($Text, [scriptblock]$Action) { & $Action }
try {
    [void][IO.Directory]::CreateDirectory((Join-Path $fixtureRoot 'INF'))
    $env:windir = $fixtureRoot
    $boundPath = Join-Path $fixtureRoot 'INF\oem42.inf'
    [IO.File]::WriteAllText($boundPath, $inf)
    $m | Add-Member files @([pscustomobject]@{ path = 'payload/kmd/bc250kmd.inf'; sha256 = (Get-Sha256 $boundPath) })
    Check ((Remove-LegacyDriverVersionTags 'test-device' $m $hash) -eq 2) 'matching bound package clears its two private tags'
    Check (($script:deleted -join ',') -ceq 'Bc250DriverVersion,Bc250ReportedDriverVersion') 'cleanup never writes Windows DriverVersion'
    $script:deleted = @()
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m ('00' * 32) } 'old loaded image cannot clear tags'
    $script:boundService = 'foreign'
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m $hash } 'foreign service cannot clear tags'
    $script:boundService = $script:ServiceName
    $script:boundInf = '..\other.inf'
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m $hash } 'bound INF must be an OEM basename'
    $script:boundInf = 'oem42.inf'
    $script:propertyVersion = '31.0.0.0'
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m $hash } 'stale device property keeps private tags'
    $script:propertyVersion = $m.kmd_version
    $script:bindingValues.DriverVersion = '31.0.0.0'
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m $hash } 'stale software key keeps private tags'
    $script:bindingValues.DriverVersion = $m.kmd_version
    $script:bindingValues.ProductPackageVersion = '0.7.216.102'
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m $hash } 'wrong product keeps private tags'
    $script:bindingValues.ProductPackageVersion = $i.product_quad
    $script:boundKey += '\unrelated'
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m $hash } 'foreign software subkey refused'
    $script:boundKey = $script:boundKey.Substring(0, $script:boundKey.LastIndexOf('\'))
    [IO.File]::AppendAllText($boundPath, '; changed')
    Refuses { Remove-LegacyDriverVersionTags 'test-device' $m $hash } 'different INF bytes keep private tags'
    Check ($script:deleted.Count -eq 0) 'every refusal was read-only'
} finally {
    $env:windir = $savedWindows
    # The generated leaf and its parent are both checked before recursive cleanup.
    $resolved = [IO.Path]::GetFullPath($fixtureRoot)
    if ([IO.Path]::GetDirectoryName($resolved) -ine [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') -or
        [IO.Path]::GetFileName($resolved) -notmatch '\Aversion-identity-[0-9a-f]{32}\z') { throw 'unsafe fixture cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
"version identity: $script:checks checks PASS"
