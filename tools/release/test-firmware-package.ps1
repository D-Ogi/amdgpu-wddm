# Host-only Driver Store firmware regression. Windows PowerShell 5.1, elevated
# for the private SYSTEM/Administrators staging ACL. No install or trust import.
param([Parameter(Mandatory)][string]$WorkRoot, [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$SysFile, [Parameter(Mandatory)][string]$FirmwareDir,
    [string]$KitVersion = '10.0.26100.0')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer\common.ps1')
$repo = Split-Path (Split-Path $PSScriptRoot)
$script:checks = 0
function Check([bool]$Ok, [string]$Why) { $script:checks++; if (-not $Ok) { throw "FAIL: $Why" } }
function Refuses([scriptblock]$Body, [string]$Why) {
    $refused = $false
    try { & $Body | Out-Null } catch { $refused = $true }
    Check $refused $Why
}
function Clone($Object) { return ($Object | ConvertTo-Json -Depth 16 | ConvertFrom-Json) }
[void][IO.Directory]::CreateDirectory($WorkRoot)
$case = Join-Path $WorkRoot ('firmware-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($case)
$source = Join-Path $case 'source'
$kmd = Join-Path $case 'package\payload\kmd'
$parent = Join-Path $case 'private'
foreach ($p in @($source, $kmd, $parent)) { [void][IO.Directory]::CreateDirectory($p) }
$pin = Get-Content (Join-Path $repo 'tools\firmware\cyan_skillfish2.json') -Raw | ConvertFrom-Json
$fwSource = (Get-Content (Join-Path $repo 'tools\release\release-sources.json') -Raw | ConvertFrom-Json).firmware
$fw = [pscustomobject]@{ delivery = 'driver-store-v1'; commit = $pin.commit; install_dir = '%13%'; files = @($pin.files) + @($fwSource.licence) }
Check (Test-DriverStoreFirmware $fw) 'pinned firmware contract'
Check (-not (Test-DriverStoreFirmware $null)) 'no firmware contract is legacy'
Check (-not (Test-DriverStoreFirmware ([pscustomobject]@{ install_dir = 'C:\BC250\firmware' }))) 'legacy folder contract'
foreach ($value in @($null, '', 'driver-store-v2')) {
    $bad = Clone $fw; $bad.delivery = $value
    Refuses { Test-DriverStoreFirmware $bad } 'invalid delivery refused'
}
$bad = Clone $fw; $bad.PSObject.Properties.Remove('delivery')
Refuses { Test-DriverStoreFirmware $bad } 'missing delivery cannot become legacy with DIRID13'
foreach ($value in @($null, '64', 1.5, -1, 0, 1048577, $true)) {
    $bad = Clone $fw; $bad.files[0].size = $value
    Refuses { Test-DriverStoreFirmware $bad } 'invalid size refused'
}
foreach ($value in @($null, 'xy', ('g' * 64))) {
    $bad = Clone $fw; $bad.files[0].sha256 = $value
    Refuses { Test-DriverStoreFirmware $bad } 'invalid SHA256 refused'
}
$bad = Clone $fw; $bad.files[0].name = $bad.files[1].name
Refuses { Test-DriverStoreFirmware $bad } 'duplicate or missing firmware member refused'
$bad = Clone $fw; $bad.files[0].name = '..\escape.bin'
Refuses { Test-DriverStoreFirmware $bad } 'traversal refused'
Copy-DriverStoreFirmware $fw $FirmwareDir $source
[IO.File]::Copy((Join-Path $repo 'docs\testing\licenses\LICENSE.amdgpu'), (Join-Path $source 'LICENSE.amdgpu'))
Check ((Test-FirmwareFolder $fw $source).Count -eq 0) 'all nine real pinned files'
$bad = Clone $fw; $bad.files[0].sha256 = '0' * 64
$unmade = Join-Path $case 'refused-copy'
Refuses { Copy-DriverStoreFirmware $bad $source $unmade } 'bad pin stops copy'
Check (-not (Test-Path -LiteralPath $unmade)) 'bad pin leaves no destination'
Refuses { Get-FirmwareStaged $fw $source $source } 'offline source cannot be its own staging directory'
Check ((Test-FirmwareFolder $fw $source).Count -eq 0) 'same-source refusal preserves inputs'
$download = Join-Path $case 'offline'
$staged = @(Get-FirmwareStaged $fw $download $source)
Check ($staged.Count -eq 9 -and (Test-FirmwareFolder $fw $download).Count -eq 0) 'offline staging checks sizes and hashes'

[IO.File]::Copy((Join-Path $repo 'driver\kmd\bc250kmd.inf'), (Join-Path $kmd 'bc250kmd.inf'))
[IO.File]::Copy($SysFile, (Join-Path $kmd 'bc250kmd.sys'))
$inf2cat = Join-Path $Kits "microsoft.windows.wdk.x64\c\bin\$KitVersion\x86\Inf2Cat.exe"
# A public package lacks the eight binaries; the OS package must not be signed
# or staged in that state. Check the actual catalog generator, then hydrate it.
& $inf2cat "/driver:$kmd" '/os:10_X64' *> (Join-Path $case 'incomplete.log')
Check ($LASTEXITCODE -ne 0) 'catalog generation refuses missing firmware'
Copy-DriverStoreFirmware $fw $source $kmd
& $inf2cat "/driver:$kmd" '/os:10_X64' *> (Join-Path $case 'complete.log')
Check ($LASTEXITCODE -eq 0) "complete Driver Store package catalog: $(Join-Path $case 'complete.log')"
Test-DriverCatalogMembers $kmd (@('bc250kmd.inf', 'bc250kmd.sys') + @(Get-DriverStoreFirmwareNames))
Check $true 'catalog contains actual INF, SYS and all eight pinned files'
$cat = Join-Path $kmd 'bc250kmd.cat'
$changed = Join-Path $case 'changed.bin'
[IO.File]::Copy((Join-Path $source 'cyan_skillfish2_ce.bin'), $changed)
[IO.File]::AppendAllText($changed, 'changed')
Check (-not [AmdgpuWddm.CatalogMembership]::Contains($cat, $changed)) 'catalog refuses changed firmware bytes'
Check (-not [AmdgpuWddm.CatalogMembership]::Contains($cat, (Join-Path $source 'LICENSE.amdgpu'))) 'catalog refuses nonmember'

$manifest = [pscustomobject]@{ firmware = $fw; files = @('bc250kmd.inf', 'bc250kmd.sys', 'bc250kmd.cat' | ForEach-Object {
    $p = Join-Path $kmd $_
    [pscustomobject]@{ path = "payload/kmd/$_"; size = (Get-Item -LiteralPath $p).Length; sha256 = Get-Sha256 $p }
}) }
# Model the public ZIP: exact INF/CAT, no firmware. Delete fixed known leaves only.
foreach ($name in Get-DriverStoreFirmwareNames) { Remove-Item -LiteralPath (Join-Path $kmd $name) }
Check (@(Get-ChildItem -LiteralPath $kmd -File).Count -eq 3) 'public payload has only INF, SYS and CAT'
$package = Join-Path $case 'package'
$scope = New-HydratedDriverPackage $manifest $package $source $parent
$stagePath = $scope.DirectoryPath
try {
    Check (@(Get-ChildItem -LiteralPath $stagePath -File).Count -eq 11) 'held hydration has exactly eleven files'
    foreach ($name in @('bc250kmd.inf', 'bc250kmd.sys', 'bc250kmd.cat') + @(Get-DriverStoreFirmwareNames)) {
        $p = Join-Path $stagePath $name
        Refuses { [IO.File]::Open($p, 'Open', 'Write', 'ReadWrite').Dispose() } "sealed file rejects write: $name"
        Refuses { [IO.File]::Delete($p) } "sealed file rejects delete: $name"
    }
    Refuses { [IO.Directory]::Move($stagePath, ($stagePath + '-renamed')) } 'held directory rejects rename'
    Refuses { $scope.Copy($changed, 'another.bin', (Get-Item -LiteralPath $changed).Length) } 'sealed stage rejects new members'
} finally { $scope.Dispose() }
Check (-not (Test-Path -LiteralPath $stagePath)) 'disposing scope removes only its private files'
$bad = Clone $manifest; $bad.files[0].sha256 = '0' * 64
Refuses { New-HydratedDriverPackage $bad $package $source $parent } 'tampered INF hash refuses hydration'
Check (@(Get-ChildItem -LiteralPath $parent -Force).Count -eq 0) 'failed hydration cleans its own directory'
$bad = Clone $manifest; $bad.files += [pscustomobject]@{ path = 'payload/kmd/other.inf'; size = 1; sha256 = '0' * 64 }
Refuses { New-HydratedDriverPackage $bad $package $source $parent } 'second INF refuses before staging'
$junction = Join-Path $case 'junction'
[void](New-Item -ItemType Junction -Path $junction -Target $parent)
try {
    Refuses { New-HydratedDriverPackage $manifest $package $source $junction } 'reparse parent refused'
} finally { [IO.Directory]::Delete($junction) }

"firmware package: $script:checks checks PASS; artifacts $case"
