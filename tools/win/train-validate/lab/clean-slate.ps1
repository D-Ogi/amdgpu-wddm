# The clean-slate installer test (2026-10-06) as three calls: remove everything of amdgpu-wddm from the lab, then
# install the release fresh from its package. The uninstaller is the INSTALLED one of the release on the lab
# (lead, 2026-10-08). The C:\BC250 harness stays. Generic: the package name and the directory it was pushed to
# are arguments, so one copy serves every train. Steps, each one call (bounded well under three minutes):
#   -Step uninstall  : the uninstaller of the installed release, run from the install root as uninstall.cmd does
#                      (<InstallRoot>\installer\uninstall.ps1, -Yes -KeepTestSigning -NoReboot -Force), then the
#                      inventory
#   -Step inventory  : what of ours is still on the machine (files, registry, driver store, tasks, certificates)
#   -Step install    : install.ps1 of the package, fresh (-Force -AcceptTestSigning -NoReboot -NoComponents -NoModels
#                      -CuMode 40). Without the two "No" switches a non-interactive run takes the extras question's
#                      default, yes, and downloads llama.cpp and Ollama (tester.28 and later);
#                      -Repair installs the same version again over itself (the update/in-use replace path)
# The restarts between the steps are separate calls (restart-now.ps1).
param([Parameter(Mandatory = $true)][ValidateSet('uninstall', 'inventory', 'install')][string]$Step,
      [Parameter(Mandatory = $true)][string]$Root,
      [string]$Name = '',
      [string]$Sha = '',
      [int]$CuMode = 40,
      [switch]$Repair)
$ErrorActionPreference = 'Continue'
if ($Step -eq 'install' -and -not $Name) { 'clean-slate: -Name is needed for the install step'; exit 2 }
$pkg = "$Root\$Name"
# The zip unpacks to <pkg>\setup.exe and <pkg>\files\ (2026-10-11): the installer scripts live under files\.
# A package built before that layout has them in <pkg> itself. Asked after the expansion, never before.
function Get-PackageFilesDir { if (Test-Path -LiteralPath "$pkg\files\manifest.json") { return "$pkg\files" } return $pkg }
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
            PackageRoot = Split-Path ([string]$package.OriginalFileName)
        }
    }
    return $ours
}

function Inventory {
    'boot {0:o}' -f (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime()
    foreach ($d in 'C:\Program Files\amdgpu-wddm', 'C:\ProgramData\amdgpu-wddm', "$env:LOCALAPPDATA\amdgpu-wddm", 'C:\Users\bc250\AppData\Local\amdgpu-wddm', 'C:\Users\bc250\AppData\Roaming\amdgpu-wddm') {
        if (Test-Path -LiteralPath $d) {
            $f = @(Get-ChildItem -LiteralPath $d -Recurse -File -Force -ErrorAction SilentlyContinue)
            'LEFT dir {0}: {1} files' -f $d, $f.Count
            $f | Select-Object -First 12 | ForEach-Object { '   ' + $_.FullName.Substring($d.Length) }
        } else { 'gone dir {0}' -f $d }
    }
    foreach ($f in "$env:windir\System32\bc250umd.dll", "$env:windir\SysWOW64\bc250umd.dll") {
        if (Test-Path -LiteralPath $f) { 'LEFT file ' + $f } else { 'gone file ' + $f } }
    foreach ($k in 'HKLM:\SOFTWARE\amdgpu-wddm', 'HKLM:\SOFTWARE\WOW6432Node\amdgpu-wddm', 'HKCU:\Software\amdgpu-wddm',
                   'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd') {
        if (Test-Path -LiteralPath $k) {
            $sub = @(Get-ChildItem -LiteralPath $k -Recurse -ErrorAction SilentlyContinue | ForEach-Object { $_.PSChildName })
            'LEFT key {0} (subkeys: {1})' -f $k, ($sub -join ',')
        } else { 'gone key ' + $k }
    }
    foreach ($k in 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers', 'HKLM:\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers') {
        $v = @((Get-Item -LiteralPath $k -ErrorAction SilentlyContinue).Property | Where-Object { $_ -like '*amdgpu-wddm*' })
        if ($v.Count) { 'LEFT vulkan {0}: {1}' -f $k, ($v -join '; ') } else { 'gone vulkan entries in ' + $k }
    }
    $mft = 'HKLM:\SOFTWARE\Classes\CLSID\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}'
    if (Test-Path -LiteralPath $mft) { 'LEFT mft clsid' } else { 'gone mft clsid' }
    $packages = @(Get-KmdDriverStorePackages)
    $ours = @($packages | ForEach-Object { '{0} {1} {2}' -f $_.Published, $_.Date, $_.Version })
    'driver store bc250kmd packages: {0} {1}' -f $ours.Count, ($ours -join '; ')
    foreach ($package in $packages) {
        # Read only packages DISM identified as our INF, never a file-name scan
        # through other vendors' packages. PnP owns their eventual removal.
        $store = Join-Path $env:windir 'System32\DriverStore\FileRepository'
        if ((Split-Path $package.PackageRoot) -ine $store -or (Split-Path $package.PackageRoot -Leaf) -notlike 'bc250kmd.inf_*') {
            throw 'unexpected DriverStore directory for the registered package'
        }
        $umd = @(Get-ChildItem -LiteralPath $package.PackageRoot -Recurse -File -Filter '*.dll')
        'LEFT DriverStore UMDs {0}: {1} files' -f $package.Published, $umd.Count
    }
    $t = @(Get-ScheduledTask -ErrorAction SilentlyContinue | Where-Object { $_.TaskName -like 'amdgpu-wddm*' } | ForEach-Object { $_.TaskName })
    'scheduled tasks amdgpu-wddm*: {0} {1}' -f $t.Count, ($t -join ', ')
    'lab emergency task present: {0}' -f [bool](Get-ScheduledTask -TaskName 'Lab emergency channel' -ErrorAction SilentlyContinue)
    foreach ($s in 'Root', 'TrustedPublisher') {
        $c = @(Get-ChildItem "Cert:\LocalMachine\$s" | Where-Object { $_.Subject -like '*amdgpu-wddm*' })
        'certificates {0}: {1} {2}' -f $s, $c.Count, (($c | ForEach-Object { $_.Subject }) -join '; ')
    }
    $dev = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_13FE*' } | Select-Object -First 1
    'gpu device: status {0}, name {1}, inf {2}, version {3}' -f $dev.Status, $dev.FriendlyName, (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data, (Get-PnpDeviceProperty -InstanceId $dev.InstanceId -KeyName DEVPKEY_Device_DriverVersion).Data
    $cls = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}\*" -ErrorAction SilentlyContinue | Where-Object { $_.UserModeDriverName -match 'bc250|amdgpu' }
    'display class keys naming our UMD: {0}' -f @($cls).Count
    'harness C:\BC250 present: {0}' -f (Test-Path 'C:\BC250\tools')
}
New-Item -ItemType Directory -Force $Root | Out-Null
switch ($Step) {
    'inventory' { Inventory }
    'uninstall' {
        $inst = 'C:\Program Files\amdgpu-wddm'
        $m = Get-Content -Raw "$inst\manifest.json" | ConvertFrom-Json
        'installed release {0} (kmd_build {1}), uninstaller {2}' -f $m.version, $m.kmd_build, (Get-FileHash "$inst\installer\uninstall.ps1").Hash.Substring(0, 8)
        '--- before'
        Inventory
        '--- uninstall'
        & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$inst\installer\uninstall.ps1" -Yes -KeepTestSigning -NoReboot -Force *>&1 | Tee-Object "$Root\uninstall-console.txt" | Select-Object -Last 40
        "uninstall exit $LASTEXITCODE"
        '--- after'
        Inventory
    }
    'install' {
        if ($Sha) { $h = (Get-FileHash "$pkg.zip" -Algorithm SHA256).Hash; if ($h -ne $Sha) { "zip hash $h, expected $Sha"; exit 2 }; "zip ok $($h.Substring(0, 8))" }
        if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
        Expand-Archive "$pkg.zip" -DestinationPath $Root -Force
        $pkgFiles = Get-PackageFilesDir
        "package files: $pkgFiles"
        $extra = @(); if ($Repair) { $extra += '-Repair' }
        & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$pkgFiles\installer\install.ps1" -Force -AcceptTestSigning -NoReboot -NoComponents -NoModels -CuMode $CuMode @extra *>&1 | Tee-Object "$Root\install-console.txt" | Select-Object -Last 30
        "install exit $LASTEXITCODE"
        '--- after install'
        Inventory
    }
}
