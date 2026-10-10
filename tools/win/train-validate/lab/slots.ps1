# Read-only: every installed payload file against the package manifest (the package table itself, not a
# hand-typed copy of it). The manifest of the extracted package is read from -Pkg, so one copy serves
# every train.
param([Parameter(Mandatory = $true)][string]$Pkg)
$ErrorActionPreference = 'Continue'
# Query package fields, never pnputil's translated display labels.
# https://learn.microsoft.com/powershell/module/dism/get-windowsdriver
function Get-KmdDriverStorePackages {
    $packages = @(Get-WindowsDriver -Online -ErrorAction Stop)
    $ours = @()
    foreach ($package in $packages) {
        if (-not $package -or -not $package.OriginalFileName) {
            throw 'Driver-store inventory has an unreadable original INF name'
        }
        $original = [IO.Path]::GetFileName([string]$package.OriginalFileName)
        if (-not [string]::Equals($original, 'bc250kmd.inf', [StringComparison]::OrdinalIgnoreCase)) { continue }
        $published = [string]$package.Driver
        $version = [string]$package.Version
        if ($published -notmatch '^oem[0-9]+\.inf$' -or $version -notmatch '^[0-9]+(?:\.[0-9]+){1,3}$' -or
            $package.Date -isnot [DateTime]) {
            throw 'Driver-store inventory has incomplete bc250kmd package details'
        }
        $ours += [pscustomobject]@{
            Published = $published
            Version = $version
            Date = $package.Date.ToString('yyyy-MM-dd', [Globalization.CultureInfo]::InvariantCulture)
        }
    }
    return $ours
}

$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
"install root  : $inst"
"package       : $Pkg"
$m = Get-Content -Raw "$Pkg\manifest.json" | ConvertFrom-Json
"package release: $($m.version), kmd_build $($m.kmd_build), kmd_abi $($m.kmd_abi)"
$im = $null
if (Test-Path -LiteralPath "$inst\manifest.json") {
    $im = Get-Content -Raw "$inst\manifest.json" | ConvertFrom-Json
    "installed manifest: $($im.version), kmd_build $($im.kmd_build)"
}
$ok = 0; $bad = 0; $absent = 0
$rows = @()
foreach ($e in ($m.files | Where-Object { $_.path -like 'payload/*' } | Sort-Object path)) {
    $rel = $e.path.Substring('payload/'.Length) -replace '/', '\'
    # the kernel driver goes to the driver store, not under the install root
    if ($rel -like 'kmd\*') { continue }
    $p = Join-Path $inst $rel
    if (-not (Test-Path -LiteralPath $p)) { $rows += ('ABSENT {0,-44} want {1}' -f $rel, $e.sha256.Substring(0,8)); $absent++; continue }
    $got = (Get-FileHash -Algorithm SHA256 -LiteralPath $p).Hash
    $len = (Get-Item -LiteralPath $p).Length
    if ($got -eq $e.sha256 -and $len -eq $e.size) {
        $rows += ('ok     {0,-44} {1}  {2} bytes' -f $rel, $got.Substring(0,8), $len); $ok++
    } else {
        $rows += ('DIFFER {0,-44} {1} ({2} bytes)  want {3} ({4} bytes)' -f $rel, $got.Substring(0,8), $len, $e.sha256.Substring(0,8), $e.size); $bad++
    }
}
$rows
"slot check: $ok match, $bad differ, $absent absent (of $($ok + $bad + $absent) payload files outside kmd\)"
'--- kernel driver in the driver store'
$packages = @(Get-KmdDriverStorePackages)
'driver store bc250kmd packages: {0}' -f $packages.Count
$packages | ForEach-Object { 'store {0} {1} {2}' -f $_.Published, $_.Date, $_.Version }
$dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
'device: status {0}, name {1}, inf {2}, version {3}' -f $dev.Status, $dev.FriendlyName, (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data, (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
'--- sidecars or stray copies under the install root'
$stray = @(Get-ChildItem -LiteralPath $inst -Recurse -File -Force | Where-Object { $_.Name -match '\.(b23|b24|cand|tester20|tester21|b20|b20orig|pre-[a-z0-9]+|inc3-out|approuter-fix-held|b15-[0-9A-F]+|b14-[0-9A-F]+|native-caps\d+\.candidate-removed)$' })
if ($stray.Count) { $stray | ForEach-Object { "STRAY $($_.FullName)" } } else { 'none' }
'--- files under the install root that the installed manifest does not name'
if ($im) {
    # the installed manifest is the package manifest, so its payload files still carry the payload/ prefix
    $named = @{}
    foreach ($e in $im.files) {
        $r = ($e.path -replace '/', '\').ToLower()
        $named[$r] = $true
        if ($r -like 'payload\*') { $named[$r.Substring('payload\'.Length)] = $true }
    }
    $extra = @(Get-ChildItem -LiteralPath $inst -Recurse -File -Force | ForEach-Object {
        $r = $_.FullName.Substring($inst.Length).TrimStart('\').ToLower()
        if (-not $named.ContainsKey($r)) { $r }
    })
    if ($extra.Count) { $extra | Select-Object -First 40 } else { 'none' }
    "installed manifest entries: $($im.files.Count)"
}
'--- TdrDelay (BD-079)'
$gd = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' -ErrorAction SilentlyContinue
'TdrDelay      = {0}' -f ($gd.TdrDelay)
'TdrDdiDelay   = {0}' -f ($gd.TdrDdiDelay)
'TdrLevel      = {0}' -f ($gd.TdrLevel)
'--- Vulkan registrations'
foreach ($k in 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers', 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers') {
    (Get-Item -LiteralPath $k -ErrorAction SilentlyContinue).Property | ForEach-Object { "$k -> $_" }
}
