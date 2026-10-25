# Host package/registry gate. No device, service, UI or HKLM writes.
param([Parameter(Mandatory)][string]$Package, [Parameter(Mandatory)][string]$WorkRoot)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'test-adl-registration.ps1')
. (Join-Path $PSScriptRoot 'installer\common.ps1')
$manifest = Get-Content -LiteralPath (Join-Path $Package 'manifest.json') -Raw | ConvertFrom-Json
if (-not (Test-DriverStoreUmdPackage $manifest)) { throw 'DriverStore package expected' }
$root = [IO.Path]::GetFullPath((Join-Path $Package 'payload\kmd'))
$failures = 0
function Check([bool]$Ok, [string]$Name) {
    if ($Ok) { Write-Output "PASS $Name" } else { Write-Output "FAIL $Name"; $script:failures++ }
}
$rows = @($manifest.driver_package.members)
Check (@($rows.member | ForEach-Object { [IO.Path]::GetFileName($_) } | Sort-Object -Unique).Count -eq $rows.Count) 'package basenames are unique'
foreach ($row in $rows) {
    $p = Join-Path $root ($row.member -replace '/', '\')
    Check ((Get-Sha256 $p) -ieq $row.sha256 -and (Get-Item -LiteralPath $p).Length -eq $row.size) "exact member $($row.member)"
    if ($p.EndsWith('.dll', [StringComparison]::OrdinalIgnoreCase)) {
        $machine = if ($row.arch -eq 'x86') { 0x14c } else { 0x8664 }
        Check ((Get-PeMachine $p) -eq $machine) "PE architecture $($row.member)"
        $exports = @(Get-PeExportNames $p)
        if ($row.member -match '/bc250d3d_router(?:32)?\.dll$') { $want = @('OpenAdapter10', 'OpenAdapter10_2') }
        elseif ($row.member -match '/amdgpu_wddm_d3d11(?:32)?\.dll$') { $want = @('OpenAdapter10_2') }
        elseif ($row.member -match '/amdgpu_wddm_d3d12(?:32)?\.dll$') { $want = @('OpenAdapter12') }
        elseif ($row.member -match '/vulkan_radeon(?:32)?\.dll$') { $want = @('vk_icdGetInstanceProcAddr') }
        elseif ($row.member -match '/atiadl(?:xx|xy32)\.dll$') { $want = @('ADL_Main_Control_Create', 'ADL2_Main_Control_Create', 'ADL_Adapter_AdapterInfo_Get') }
        else { $want = @() }
        foreach ($name in $want) { Check ($exports -contains $name) "undecorated $name in $($row.member)" }
    }
    if ($p.EndsWith('.json', [StringComparison]::OrdinalIgnoreCase)) {
        $j = Get-Content -LiteralPath $p -Raw | ConvertFrom-Json
        $dll = if ($row.arch -eq 'x86') { '.\vulkan_radeon32.dll' } else { '.\vulkan_radeon.dll' }
        Check ($j.ICD.library_path -ceq $dll) "relative Vulkan library $($row.member)"
    }
    $component = @($manifest.components | Where-Object { $_.package_path -ceq $row.path })
    Check ($component.Count -eq 1 -and $component[0].install_path -ceq ('DriverStore (bc250kmd.inf)\' + ($row.member -replace '/', '\'))) "component placement $($row.member)"
    Check (-not (Test-Path -LiteralPath (Join-Path $Package $row.source))) "no Program Files payload duplicate $($row.source)"
}
# Check the persisted registry representation, not just a printed plan. The
# unique test key is under HKCU and is removed in finally, including on failure.
$key = 'HKCU:\Software\amdgpu-driverstore-test-' + [Guid]::NewGuid().ToString('N')
try {
    [void](New-Item -Path "$key\class" -Force)
    foreach ($row in @(Get-DriverPackageRegistration $manifest.driver_package $root)) {
        New-ItemProperty -LiteralPath "$key\class" -Name $row.name -Value $row.value -PropertyType $row.kind | Out-Null
    }
    foreach ($row in @(Get-DriverPackageSubkeyRegistration $manifest.driver_package)) {
        [void](New-Item -Path "$key\class\$($row.subkey)" -Force)
        New-ItemProperty -LiteralPath "$key\class\$($row.subkey)" -Name $row.name -Value $row.value -PropertyType $row.kind | Out-Null
    }
    $problems = @(Test-DriverPackageRegistration $manifest "$key\class" $root)
    Check ($problems.Count -eq 0) "INF registry round trip ($($problems -join '; '))"
    foreach ($row in @(Get-DriverPackageSubkeyRegistration $manifest.driver_package)) {
        Set-ItemProperty -LiteralPath "$key\class\$($row.subkey)" -Name $row.name -Value @('foreign.dll', $row.value[1])
        $problems = @(Test-DriverPackageRegistration $manifest "$key\class" $root)
        Check ($problems -contains "$($row.subkey)\$($row.name) differs from the bound package INF") 'wrong ADL CopyToVm source refused'
        Set-ItemProperty -LiteralPath "$key\class\$($row.subkey)" -Name $row.name -Value $row.value
    }
    # Wrong architecture and missing slot must fail, even though files exist.
    Set-ItemProperty -LiteralPath "$key\class" -Name UserModeDriverNameWow -Value @((Get-ItemProperty -LiteralPath "$key\class").UserModeDriverName)
    $problems = @(Test-DriverPackageRegistration $manifest "$key\class" $root)
    Check (@($problems | Where-Object { $_ -like 'UserModeDriverNameWow differs*' }).Count -eq 1) 'x64 paths in Wow registration refused'
    Remove-ItemProperty -LiteralPath "$key\class" -Name VulkanDriverName
    $problems = @(Test-DriverPackageRegistration $manifest "$key\class" $root)
    Check ($problems -contains 'VulkanDriverName is missing') 'missing Vulkan registration refused'
    $legacy = Join-Path $WorkRoot 'previous-install'
    foreach ($k in 'sw\DesktopRouter','sw\AppRouter','vk','vk32') { [void](New-Item -Path "$key\$k" -Force) }
    New-ItemProperty -LiteralPath "$key\sw\DesktopRouter" -Name CpuUmdPath -Value (Join-Path $legacy 'desktop\bc250d3d.dll') -PropertyType String | Out-Null
    New-ItemProperty -LiteralPath "$key\sw\AppRouter" -Name Mode -Value 'gpu-default' -PropertyType String | Out-Null
    New-ItemProperty -LiteralPath "$key\vk" -Name (Join-Path $legacy 'vulkan\radeon_icd.json') -Value 0 -PropertyType DWord | Out-Null
    New-ItemProperty -LiteralPath "$key\vk" -Name 'C:\other-driver\icd.json' -Value 0 -PropertyType DWord | Out-Null
    $values = @(Get-LegacyUmdValues -InstallRoot $legacy -SoftwareKey "$key\sw" -KhronosKey "$key\vk" -KhronosKeyWow "$key\vk32")
    Check ($values.Count -eq 2) 'migration selects exact owned values'
    Remove-LegacyUmdValues -Rows $values
    Remove-LegacyUmdValues -Rows $values
    Check ((Get-Item -LiteralPath "$key\vk").GetValueNames() -contains 'C:\other-driver\icd.json') 'foreign Vulkan entry retained'
    Check ((Get-ItemProperty -LiteralPath "$key\sw\AppRouter").Mode -eq 'gpu-default') 'application policy retained'
    Check ((Get-Item -LiteralPath "$key\sw\DesktopRouter").GetValueNames() -notcontains 'CpuUmdPath') 'absolute CPU path removed, resume is idempotent'
    # Rollback restores only owned values to their original keys. External
    # override files are deliberately not revived by a package rollback.
    $rollback = @($values | Where-Object { $_.kind -eq 'DWord' })
    Restore-LegacyUmdValues -Rows $rollback -InstallRoot $legacy -SoftwareKey "$key\sw" -KhronosKey "$key\vk" -KhronosKeyWow "$key\vk32"
    Check ((Get-Item -LiteralPath "$key\vk").GetValueNames() -contains (Join-Path $legacy 'vulkan\radeon_icd.json')) 'rollback restores the exact legacy ICD discovery entry'
    $badRow = [pscustomobject]@{key="$key\unrelated"; name=$rollback[0].name; kind='DWord'; value=0}
    $refused=$false
    try { Restore-LegacyUmdValues -Rows @($badRow) -InstallRoot $legacy -SoftwareKey "$key\sw" -KhronosKey "$key\vk" -KhronosKeyWow "$key\vk32" } catch { $refused=$true }
    Check ($refused -and -not (Test-Path -LiteralPath "$key\unrelated")) 'rollback refuses a redirected registry key'
    $old=[pscustomobject]@{package_path='payload/desktop/bc250d3d.dll';path=(Join-Path $legacy 'desktop\bc250d3d.dll');sha256=('A'*64);from_version='old'}
    $pending=@(Get-RetiredUmdFilePlan -InstallRoot $legacy -Rows @($old,$old))
    Check ($pending.Count -eq 1 -and $pending[0].sha256 -eq $old.sha256) 'restart retains and deduplicates the previous manifest file plan'
    $old.path=Join-Path $WorkRoot 'unrelated.dll'
    $refused=$false
    try { Get-RetiredUmdFilePlan -InstallRoot $legacy -Rows @($old) | Out-Null } catch { $refused=$true }
    Check $refused 'retired UMD destination cannot leave its package slot'
} finally {
    if ($key -notmatch '^HKCU:\\Software\\amdgpu-driverstore-test-[0-9a-f]{32}$') { throw 'test cleanup key escaped scope' }
    if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Recurse -Force }
}
if ($failures) { throw "$failures DriverStore package checks failed" }
