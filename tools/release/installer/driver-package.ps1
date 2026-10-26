# The INF owns adapter registration. The installer reads the package identity
# from the bound device, never by searching FileRepository or remembering a
# directory from the previous release. No function here writes adapter paths.
function Test-DriverStoreUmdPackage {
    param($Manifest)
    if (-not $Manifest -or -not $Manifest.PSObject.Properties['driver_package']) { return $false }
    $p = $Manifest.driver_package
    if (-not $p -or $p.schema -ne 1 -or $p.root -cne 'payload/kmd' -or -not @($p.members).Count) {
        throw 'invalid DriverStore user-mode package contract'
    }
    return $true
}
function Get-DriverPackageRoot {
    param([Parameter(Mandatory)][string]$ClassKey)
    $item = Get-Item -LiteralPath $ClassKey -ErrorAction SilentlyContinue
    if (-not $item -or $item.GetValueNames() -notcontains 'AmdgpuDriverPackage') { return $null }
    if ($item.GetValueKind('AmdgpuDriverPackage') -ne [Microsoft.Win32.RegistryValueKind]::String) {
        throw 'AmdgpuDriverPackage must be REG_SZ'
    }
    $path = [string]$item.GetValue('AmdgpuDriverPackage')
    if ($path -notmatch '\A[A-Za-z]:\\' -or $path.Contains('/') -or $path.Contains('..') -or
        $path.Substring(2).Contains(':') -or $path.Contains('%')) { throw 'invalid DriverStore package root' }
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\')
    $store = [IO.Path]::GetFullPath((Join-Path $env:windir 'System32\DriverStore\FileRepository')).TrimEnd('\')
    if (-not [string]::Equals((Split-Path $full), $store, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path $full -Leaf) -notmatch '\Abc250kmd\.inf_[a-z0-9_]+\z') {
        throw 'bound UMD package is not a bc250kmd FileRepository directory'
    }
    foreach ($name in 'bc250kmd.inf','bc250kmd.sys') {
        if (-not (Test-Path -LiteralPath (Join-Path $full $name) -PathType Leaf)) { throw "bound UMD package lacks $name" }
    }
    return $full
}
function Get-DriverPackageRegistration {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)][string]$Root)
    foreach ($p in $Package.registration.PSObject.Properties) {
        $v = $p.Value
        if ($v.kind -eq 'DWord') { $value = [int]$v.value }
        elseif ($v.kind -in @('String','MultiString')) {
            $values = @($v.value | ForEach-Object {
                $s = [string]$_
                if ($s -eq '<>') { $s }
                elseif ($s -eq '%13%') { $Root }
                elseif ($s.StartsWith('%13%\', [StringComparison]::Ordinal)) {
                    $member = $s.Substring(5)
                    if ($member -notmatch '\A[A-Za-z0-9_-]+(?:[.\\][A-Za-z0-9_-]+)*\z' -or $member.Contains('..')) {
                        throw 'invalid DriverStore registration member'
                    }
                    Join-Path $Root $member
                } else { throw 'adapter registration must use DIRID 13' }
            })
            if ($v.kind -eq 'String') {
                if ($values.Count -ne 1) { throw 'REG_SZ requires one value' }
                $value = $values[0]
            } else { $value = [string[]]$values }
        } else { throw 'unsupported adapter registry type' }
        [pscustomobject]@{name=$p.Name; kind=[string]$v.kind; value=$value}
    }
}
# CopyToVm pairs apply to GPU-P guests only, not the physical host's DLL search.
# The host uses the two verified ADL-only DriverStore directories on PATH.
# These pairs remain relative and must not pass through the %13% expansion.
function Get-DriverPackageSubkeyRegistration {
    param([Parameter(Mandatory)]$Package)
    $expected = @()
    foreach ($spec in @(
        @('CopyToVmWhenNewer', 'adl/atiadlxx.dll', 'atiadlxx.dll', 'x64'),
        @('CopyToVmWhenNewerWow64', 'wow64/adl/atiadlxy.dll', 'atiadlxy.dll', 'x86'))) {
        $member = @($Package.members | Where-Object { $_.member -ceq $spec[1] })
        if ($member.Count) {
            if ($member.Count -ne 1 -or $member[0].arch -cne $spec[3]) { throw 'invalid ADL member architecture' }
            $expected += [pscustomobject]@{subkey=$spec[0]; name='AmdgpuAdl'; kind='MultiString';
                value=[string[]]@(($spec[1] -replace '/', '\'), $spec[2])}
        }
    }
    $actual = @()
    if ($Package.PSObject.Properties['subkey_registration']) { $actual = @($Package.subkey_registration) }
    if ($actual.Count -ne $expected.Count) { throw 'ADL CopyToVm registration count differs from its members' }
    foreach ($want in $expected) {
        $match = @($actual | Where-Object { $_.subkey -ceq $want.subkey })
        if ($match.Count -ne 1) { throw 'missing or duplicate ADL CopyToVm subkey' }
        $row = $match[0]
        if ($row.name -cne $want.name -or $row.kind -cne $want.kind -or
            @($row.value).Count -ne 2 -or $row.value[0] -cne $want.value[0] -or $row.value[1] -cne $want.value[1]) {
            throw 'invalid ADL CopyToVm source/destination pair'
        }
    }
    $expected
}
function Test-DriverPackageSubkeyRegistration {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)][string]$ClassKey)
    foreach ($row in @(Get-DriverPackageSubkeyRegistration -Package $Package)) {
        $label = "$($row.subkey)\$($row.name)"
        $key = Get-Item -LiteralPath (Join-Path $ClassKey $row.subkey) -ErrorAction SilentlyContinue
        if (-not $key -or $key.GetValueNames() -notcontains $row.name) { "$label is missing"; continue }
        if ($key.GetValueKind($row.name).ToString() -ne $row.kind) { "$label has the wrong registry type" }
        if (-not (Test-RegistryValueSame $key.GetValue($row.name) $row.value)) { "$label differs from the bound package INF" }
    }
}
# Both names are unique, so the x86 member keeps the name AGS loads by name.
# No system DLL is overwritten; an existing vendor DLL in System32 wins.
function Get-AdlSearchPaths {
    param([Parameter(Mandatory)]$Package, [Parameter(Mandatory)][string]$Root)
    foreach ($spec in @(@('adl/atiadlxx.dll','x64'), @('wow64/adl/atiadlxy.dll','x86'))) {
        $rows = @($Package.members | Where-Object { $_.member -ceq $spec[0] })
        if (-not $rows.Count) { continue }
        if ($rows.Count -ne 1 -or $rows[0].arch -cne $spec[1]) { throw 'Invalid host ADL architecture' }
        Split-Path (Join-Path $Root ($spec[0] -replace '/', '\'))
    }
}

# State can remove only our exact ADL directories, never an arbitrary PATH entry.
# Files may already be gone at uninstall, so this validates names, not existence.
function Assert-AdlSearchPath([string]$Entry) {
    $prefix = [IO.Path]::GetFullPath((Join-Path $env:windir 'System32\DriverStore\FileRepository')).TrimEnd('\') + '\'
    if (-not $Entry.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
        $Entry.Substring($prefix.Length) -notmatch '\Abc250kmd\.inf_[a-z0-9_]+\\(?:wow64\\)?adl\z') {
        throw 'ADL search path is outside the package ADL directories'
    }
}

function Get-AdlMachinePathPlan {
    param([string]$Current, [string[]]$Previous = @(), [string[]]$Desired = @())
    foreach ($entry in @($Previous) + @($Desired)) { Assert-AdlSearchPath $entry }
    $next = $Current
    foreach ($entry in $Previous) {
        if ($Desired -notcontains $entry) { $next = Remove-PathEntry $next $entry }
    }
    foreach ($entry in $Desired) { $next = Add-PathEntry $next $entry }
    $next
}

function Test-DriverPackageRegistration {
    param([Parameter(Mandatory)]$Manifest, [Parameter(Mandatory)][string]$ClassKey,
        [Parameter(Mandatory)][string]$Root)
    Initialize-FirmwarePackageNative
    $key = Get-Item -LiteralPath $ClassKey -ErrorAction Stop
    foreach ($row in @(Get-DriverPackageRegistration -Package $Manifest.driver_package -Root $Root)) {
        if ($key.GetValueNames() -notcontains $row.name) { "$($row.name) is missing"; continue }
        if ($key.GetValueKind($row.name).ToString() -ne $row.kind) { "$($row.name) has the wrong registry type" }
        if (-not (Test-RegistryValueSame $key.GetValue($row.name) $row.value)) { "$($row.name) differs from the bound package INF" }
    }
    Test-DriverPackageSubkeyRegistration -Package $Manifest.driver_package -ClassKey $ClassKey
    foreach ($row in @($Manifest.driver_package.members)) {
        $relative = [AmdgpuWddm.PackageStage]::MemberPath([string]$row.member)
        $path = Join-Path $Root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { "missing DriverStore member: $relative"; continue }
        if ((Get-Item -LiteralPath $path).Length -ne $row.size -or (Get-Sha256 $path) -ine $row.sha256) {
            "DriverStore member differs from the manifest: $relative"
        }
    }
}
# The first migration snapshot stays with the BD-152 continuation across boots.
function Get-RetiredUmdFilePlan {
    param([string]$InstallRoot, [object[]]$Rows = @())
    $seen = @{}
    foreach ($row in $Rows) {
        $slot = [string]$row.package_path
        if ($slot -notmatch '\Apayload/(desktop|d3d11|d3d12|vulkan|wow64|opengl|mft)/[A-Za-z0-9_./-]+\z') { continue }
        if ([string]$row.sha256 -notmatch '\A[0-9a-fA-F]{64}\z') { throw 'invalid retired UMD hash' }
        $path = Get-InstalledPathOfPackageFile -PackagePath $slot -InstallRoot $InstallRoot
        if (-not $path -or -not [string]::Equals($path, [string]$row.path, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'retired UMD destination differs from its install slot'
        }
        if (-not $seen.ContainsKey($path)) {
            $seen[$path] = $true
            [pscustomobject]@{path=$path; package_path=$slot; sha256=[string]$row.sha256; from_version=[string]$row.from_version}
        }
    }
}

# Only exact install-root ICD names and our six router path overrides qualify.
# Mode, allow/deny lists, application profiles and foreign Vulkan entries stay.
function Get-LegacyUmdValues {
    param([Parameter(Mandatory)][string]$InstallRoot, [string]$SoftwareKey = $script:SoftwareKey,
        [string]$KhronosKey = $script:KhronosKey, [string]$KhronosKeyWow = $script:KhronosKeyWow)
    $slots = @(
        @{key="$SoftwareKey\DesktopRouter"; names=@('CpuUmdPath','CpuUmdPathWow','HostedUmdPath','HostedUmdPathWow')},
        @{key="$SoftwareKey\AppRouter"; names=@('GpuUmdPath','GpuUmdPathWow')},
        @{key=$KhronosKey; names=@((Join-Path $InstallRoot 'vulkan\radeon_icd.json'))},
        @{key=$KhronosKeyWow; names=@((Join-Path $InstallRoot 'wow64\vulkan\radeon_icd.json'))})
    foreach ($slot in $slots) {
        $key = Get-Item -LiteralPath $slot.key -ErrorAction SilentlyContinue
        foreach ($name in $slot.names) {
            if ($key -and $key.GetValueNames() -contains $name) {
                [pscustomobject]@{key=$slot.key; name=$name; kind=$key.GetValueKind($name).ToString();
                    value=$key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}
            }
        }
    }
}
function Remove-LegacyUmdValues {
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows)
    foreach ($row in $Rows) {
        $key = Get-Item -LiteralPath $row.key -ErrorAction SilentlyContinue
        if (-not $key -or $key.GetValueNames() -notcontains $row.name) { continue }
        if ($key.GetValueKind($row.name).ToString() -ne $row.kind -or
            -not (Test-RegistryValueSame $key.GetValue($row.name) $row.value)) {
            throw "UMD override changed during migration: $($row.key) $($row.name)"
        }
        Remove-ItemProperty -LiteralPath $row.key -Name $row.name -ErrorAction Stop
    }
}
function Restore-LegacyUmdValues {
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Rows,
        [Parameter(Mandatory)][string]$InstallRoot, [string]$SoftwareKey = $script:SoftwareKey,
        [string]$KhronosKey = $script:KhronosKey, [string]$KhronosKeyWow = $script:KhronosKeyWow)
    # Restoring a lab override into an old FileRepository would defeat rollback.
    # The legacy installer writes its known CPU/GPU paths; only the old exact
    # Program Files paths are eligible here. Custom lab overrides must be re-armed.
    foreach ($row in $Rows) {
        if ($row.name -in @('CpuUmdPath','CpuUmdPathWow','GpuUmdPath','GpuUmdPathWow','HostedUmdPath','HostedUmdPathWow')) {
            $group = if ($row.name -like 'Gpu*') { 'AppRouter' } else { 'DesktopRouter' }
            if ($row.key -ine "$SoftwareKey\$group" -or $row.kind -ne 'String' -or $row.value -isnot [string]) {
                throw 'invalid retained router override'
            }
            $path = [string]$row.value
            $prefix = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\') + '\'
            if (-not [IO.Path]::IsPathRooted($path) -or
                -not [string]::Equals([IO.Path]::GetFullPath($path), $path, [StringComparison]::OrdinalIgnoreCase) -or
                -not $path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or
                -not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        } else {
            $expectedKey = if ($row.name -ieq (Join-Path $InstallRoot 'vulkan\radeon_icd.json')) { $KhronosKey }
                elseif ($row.name -ieq (Join-Path $InstallRoot 'wow64\vulkan\radeon_icd.json')) { $KhronosKeyWow }
                else { throw 'invalid retained Vulkan entry' }
            if ($row.key -ine $expectedKey -or $row.kind -ne 'DWord' -or $row.value -notin @(0,1)) {
                throw 'invalid retained Vulkan entry'
            }
        }
        Initialize-RegistryKey $row.key
        New-ItemProperty -LiteralPath $row.key -Name $row.name -Value $row.value -PropertyType $row.kind -Force | Out-Null
    }
}
