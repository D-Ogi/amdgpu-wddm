# amdgpu-wddm tester installer, v0. Run install.cmd as administrator from the unpacked package folder.
#
# Three phases, resumed automatically from %ProgramData%\amdgpu-wddm\installer\state.json:
#   1. preflight, System Restore point, test signing on (asks first), then a restart. Skipped when test signing is
#      already active in the running boot.
#   2. certificate, driver package, user-mode drivers, registry, firmware, start-confirm task, control app; restart.
#   3. verify: driver bound, version, start health, D3D12 device at feature level 12_1, Vulkan enumerates the GPU.
# -DryRun runs every check and prints every change without making it. -Verify runs phase 3 only.
# No network access and no dependency on any other computer.
[CmdletBinding()]
param(
    [switch]$DryRun,
    [switch]$Verify,
    [switch]$Force,                         # install even over a development-lab installation (not supported)
    [switch]$NoReboot,                      # never restart; tell the tester to do it
    [switch]$AcceptTestSigning,             # unattended: answer YES to the test-signing question
    [ValidateSet('', 'HaveKey', 'Suspend')][string]$BitLocker = '',
    [ValidateRange(1000, 2000)][int]$DpmMaxMHz = 1500,
    [ValidateSet(0, 24, 40)][int]$CuMode = 0,  # 0 = leave unset (driver default, 24 CUs)
    [string]$InstallRoot = (Join-Path $env:ProgramFiles 'amdgpu-wddm'),
    [switch]$NoControlApp,
    [switch]$DryRunIgnoreBoard              # host test only, honoured with -DryRun: walk all phases on a PC without a BC-250
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$package = Split-Path -Parent $here
. (Join-Path $here 'common.ps1')
$script:DryRunMode = [bool]$DryRun

# ---- 64-bit, elevated ------------------------------------------------------------------------------------------
if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) {
    $ps = Join-Path $env:windir 'sysnative\WindowsPowerShell\v1.0\powershell.exe'
    & $ps -NoProfile -ExecutionPolicy Bypass -File $MyInvocation.MyCommand.Path @PSBoundParameters
    exit $LASTEXITCODE
}
if (-not $DryRun -and -not (Test-IsAdmin)) {
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $MyInvocation.MyCommand.Path + '"'))
    foreach ($k in $PSBoundParameters.Keys) {
        $v = $PSBoundParameters[$k]
        if ($v -is [switch]) { if ($v) { $argList += "-$k" } } else { $argList += "-$k"; $argList += ('"' + [string]$v + '"') }
    }
    Write-Host 'Administrator rights are needed: Windows will ask for them now.'
    Start-Process -FilePath powershell.exe -ArgumentList $argList -Verb RunAs | Out-Null
    exit 0
}
if (-not $DryRun) {
    [void][IO.Directory]::CreateDirectory($script:StateDir)
    $script:LogPath = Join-Path $script:StateDir ('install-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.log')
}

Write-Host "amdgpu-wddm tester installer$(if ($DryRun) { ' - DRY RUN, nothing will be changed' })" -ForegroundColor White
Write-Info "package: $package"
$early = Read-InstallState
if ($early -and $early.install_root) { $InstallRoot = $early.install_root }
Write-Info "install root: $InstallRoot"

# ---- preflight -----------------------------------------------------------------------------------------------
# Every check runs, in a dry run too. Severity: fail stops the install, warn needs attention, ok is fine.
function Invoke-Preflight {
    $checks = New-Object System.Collections.ArrayList
    function Add-Check([string]$Name, [string]$Severity, [string]$Detail) { [void]$checks.Add([pscustomobject]@{ check = $Name; result = $Severity; detail = $Detail }) }

    $integrity = Test-PackageManifest -PackageRoot $package
    if ($integrity.ok) { Add-Check 'package integrity' 'ok' $integrity.detail } else { Add-Check 'package integrity' 'fail' $integrity.detail }
    $script:Manifest = $integrity.manifest

    if (Test-IsAdmin) { Add-Check 'administrator' 'ok' 'elevated' }
    elseif ($DryRun) { Add-Check 'administrator' 'warn' 'not elevated: Secure Boot, BitLocker and boot options read as unknown in this dry run' }
    else { Add-Check 'administrator' 'fail' 'not elevated' }

    $os = Get-CimInstance Win32_OperatingSystem
    $build = [int]$os.BuildNumber
    if (-not [Environment]::Is64BitOperatingSystem) { Add-Check 'Windows' 'fail' "$($os.Caption) is 32-bit; x64 is required" }
    elseif ($build -lt 22000) { Add-Check 'Windows' 'fail' "$($os.Caption) build $build; Windows 11 (build 22000 or later) is required" }
    else { Add-Check 'Windows' 'ok' "$($os.Caption) build $build" }

    $dev = @(Get-Bc250Device)
    $board = (Get-CimInstance Win32_BaseBoard -ErrorAction SilentlyContinue)
    $boardText = if ($board) { "$($board.Manufacturer) $($board.Product)".Trim() } else { 'unknown board' }
    if ($dev.Count -eq 0 -and $DryRun -and $DryRunIgnoreBoard) { Add-Check 'BC-250 GPU' 'warn' "no device $($script:HardwareIdPrefix) ($boardText); -DryRunIgnoreBoard: the dry run continues to show every phase" }
    elseif ($dev.Count -eq 0) { Add-Check 'BC-250 GPU' 'fail' "no device $($script:HardwareIdPrefix) on this computer ($boardText). This package is for the ASRock BC-250 only." }
    elseif ($dev.Count -gt 1) { Add-Check 'BC-250 GPU' 'fail' "$($dev.Count) matching devices; exactly one is supported" }
    else {
        $script:Device = $dev[0]
        $svc = Get-DeviceServiceName -InstanceId $dev[0].DeviceID
        $script:DeviceService = $svc
        Add-Check 'BC-250 GPU' 'ok' "found ($boardText), current driver service: $(if ($svc) { $svc } else { 'none' })"
    }

    $sb = Get-SecureBootState
    $script:SecureBoot = $sb
    if ($sb -eq 'on') { Add-Check 'Secure Boot' 'fail' 'on: test signing cannot be enabled. Turn Secure Boot off in the BIOS setup first (see INSTALL.md, section Secure Boot).' }
    elseif ($sb -eq 'unknown') { Add-Check 'Secure Boot' 'warn' 'state unknown' }
    else { Add-Check 'Secure Boot' 'ok' $sb }

    $tsActive = Get-TestSigningActive
    $tsNext = Get-TestSigningConfigured
    $script:TestSigningActive = $tsActive
    $script:TestSigningConfigured = $tsNext
    Add-Check 'test signing' 'ok' ("active in this boot: $tsActive; set for the next boot: $(if ($null -eq $tsNext) { 'unknown' } else { $tsNext })")

    $bl = Get-BitLockerState
    $script:BitLockerState = $bl
    if ($bl -eq 'on') { Add-Check 'BitLocker' 'warn' "protection on for $($env:SystemDrive): changing the boot options makes Windows ask for the recovery key at the next start. You must have the key, or let the installer suspend BitLocker for two restarts." }
    elseif ($bl -eq 'unknown') { Add-Check 'BitLocker' 'warn' 'state unknown: if BitLocker is on, have the recovery key ready' }
    else { Add-Check 'BitLocker' 'ok' $bl }

    $hvci = Get-MemoryIntegrityState
    if ($hvci -eq 'on') { Add-Check 'Memory integrity' 'warn' 'on: if the driver does not start, turn off Core isolation > Memory integrity (see INSTALL.md)' }
    else { Add-Check 'Memory integrity' 'ok' $hvci }

    $vc = @(Get-VcRuntimeMissing)
    if ($vc.Count) { Add-Check 'Visual C++ runtime' 'fail' ("missing in System32: $($vc -join ', '). Install the Microsoft Visual C++ Redistributable for Visual Studio 2015-2022 (x64) from https://aka.ms/vs/17/release/vc_redist.x64.exe (on any computer with a network, then copy it here), then run the installer again.") }
    else { Add-Check 'Visual C++ runtime' 'ok' 'present' }

    $drive = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$($env:SystemDrive)'"
    $freeGb = [math]::Round($drive.FreeSpace / 1GB, 1)
    if ($freeGb -lt 2) { Add-Check 'free space' 'fail' "$freeGb GB free on $($env:SystemDrive); 2 GB needed" } else { Add-Check 'free space' 'ok' "$freeGb GB free on $($env:SystemDrive)" }

    if (Test-LabInstallPresent) {
        if ($Force) { Add-Check 'existing installation' 'warn' 'a development-lab installation (C:\BC250\m1x) is present; -Force given' }
        else { Add-Check 'existing installation' 'fail' 'a development-lab installation (C:\BC250\m1x) is present. The release installer does not change a lab machine.' }
    }
    $st = Read-InstallState
    $script:State = $st
    if ($st) { Add-Check 'installer state' 'ok' "phase $($st.phase) since $($st.updated_utc)" }
    elseif ($script:DeviceService -eq $script:ServiceName -and -not $Force) { Add-Check 'existing installation' 'fail' 'bc250kmd is already installed without installer state; run uninstall.cmd first' }

    return $checks
}

function Set-ResumeAtLogon([string]$Command) {
    Invoke-Change "RunOnce entry '$($script:RunOnceName)' -> $Command (runs at the next logon)" {
        New-Item -Path $script:RunOnceKey -Force | Out-Null
        Set-ItemProperty -LiteralPath $script:RunOnceKey -Name $script:RunOnceName -Value ('"' + $Command + '"')
    } | Out-Null
}

# Verification needs no preflight: it reads the installed copy (verify.cmd in the install root) or the package.
if ($Verify -or ($early -and $early.phase -eq 'installed' -and -not $DryRun)) {
    $m = Join-Path $package 'manifest.json'
    if (Test-Path -LiteralPath $m) { $script:Manifest = Get-Content -LiteralPath $m -Raw | ConvertFrom-Json }
    $state = $early
    if (-not $state) { $state = [pscustomobject]@{ schema = 1; phase = 'unknown' } }
    $script:VerifyOnly = $true
} else {
Write-Step 'Preflight'
try { $checks = Invoke-Preflight }
catch {
    Write-Fail "preflight error: $($_.Exception.Message) $($_.InvocationInfo.PositionMessage)"
    Write-Host 'Preflight could not finish. Nothing was changed.' -ForegroundColor Red
    exit 2
}
foreach ($c in $checks) {
    $color = switch ($c.result) { 'ok' { 'Green' } 'warn' { 'Yellow' } default { 'Red' } }
    Write-Host ('   [{0,-4}] {1,-22} {2}' -f $c.result, $c.check, $c.detail) -ForegroundColor $color
    Write-Log ('   [{0}] {1}: {2}' -f $c.result, $c.check, $c.detail)
}
$failed = @($checks | Where-Object { $_.result -eq 'fail' })
if ($failed.Count) {
    Write-Host ''
    Write-Host "Preflight refused the installation: $($failed.Count) check(s) failed. Nothing was changed." -ForegroundColor Red
    exit 2
}

$state = $script:State
if (-not $state) { $state = [pscustomobject]@{ schema = 1; phase = 'new'; created_utc = [DateTime]::UtcNow.ToString('o'); updated_utc = $null } }
}
function Save-Phase([string]$Phase) {
    Set-StateValue $state 'phase' $Phase
    Set-StateValue $state 'updated_utc' ([DateTime]::UtcNow.ToString('o'))
    Save-InstallState $state
}
function Request-Restart([string]$Why) {
    Write-Host ''
    Write-Host "A restart is needed: $Why" -ForegroundColor White
    if ($script:DryRunMode -or $NoReboot) { Write-Info 'restart the computer yourself; the installer continues after you log on again'; return }
    $go = Read-Confirmation -Question 'Restart now?' -Expect 'Y'
    if ($go) { Restart-Computer -Force } else { Write-Info 'restart later; the installer continues after you log on again' }
}

# ---- phase 3: verify -------------------------------------------------------------------------------------------
function Invoke-Verify {
    Write-Step 'Verify'
    $results = New-Object System.Collections.ArrayList
    function Add-Result([string]$Name, [bool]$Ok, [string]$Detail) {
        [void]$results.Add([pscustomobject]@{ check = $Name; pass = $Ok; detail = $Detail })
        if ($Ok) { Write-Host ('   [pass] {0,-20} {1}' -f $Name, $Detail) -ForegroundColor Green } else { Write-Host ('   [FAIL] {0,-20} {1}' -f $Name, $Detail) -ForegroundColor Red }
        Write-Log ('   verify {0}: {1} {2}' -f $Name, $Ok, $Detail)
    }
    $dev = @(Get-Bc250Device)
    if ($dev.Count -ne 1) { Add-Result 'device' $false 'BC-250 GPU not found'; return $results }
    $id = $dev[0].DeviceID
    $svc = Get-DeviceServiceName -InstanceId $id
    $pnp = Get-PnpDevice -InstanceId $id
    Add-Result 'driver bound' (($svc -eq $script:ServiceName) -and ($pnp.Status -eq 'OK')) "service $svc, status $($pnp.Status), problem code $($dev[0].ConfigManagerErrorCode)"
    $drv = Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID='$($id -replace '\\', '\\')'" -ErrorAction SilentlyContinue
    $want = $null
    if ($script:Manifest) { $want = $script:Manifest.kmd_version }
    Add-Result 'driver version' (($null -ne $drv) -and ($drv.DriverVersion -eq $want)) "installed $($drv.DriverVersion), package $want"
    $cli = Join-Path $InstallRoot 'tools\bc250kmd_cli.exe'
    if (Test-Path -LiteralPath $cli) {
        $h = (Invoke-Native $cli @('health', 'read')).text.Trim()
        $flags = $null
        if ($h -match 'flags=([0-9]+)') { $flags = [int]$Matches[1] }
        Add-Result 'start health' (($null -ne $flags) -and (($flags -band 1) -eq 1)) $h
    } else { Add-Result 'start health' $false "missing $cli" }
    $uc = (Get-ItemProperty -LiteralPath $script:ParametersKey -Name UnconfirmedStarts -ErrorAction SilentlyContinue).UnconfirmedStarts
    Add-Result 'boot-loop guard' (($null -eq $uc) -or ([int]$uc -lt 2)) "UnconfirmedStarts $uc (the start-confirm task resets it after each logon)"

    # D3D12 through the system runtime, as an application sees it.
    $caps = Join-Path $InstallRoot 'tools\amdgpu_wddm_d3d12caps.exe'
    $vdir = Join-Path $script:StateDir 'verify'
    [void][IO.Directory]::CreateDirectory($vdir)
    $index = $null
    try {
        $first = Join-Path $vdir 'd3d12caps-0.json'
        [void](Invoke-Native $caps @('0', $first))
        $j = Get-Content -LiteralPath $first -Raw | ConvertFrom-Json
        foreach ($p in $j.dxgi.adapters.PSObject.Properties) {
            $d = $p.Value.GetDesc1
            if ($d.VendorId -eq '0x1002' -and $d.DeviceId -eq '0x13FE') { $index = [int]$p.Name; break }
        }
        if ($null -eq $index) { Add-Result 'D3D12 FL 12_1' $false 'no DXGI adapter 1002:13FE' }
        else {
            $out = Join-Path $vdir "d3d12caps-$index.json"
            if ($index -ne 0) { [void](Invoke-Native $caps @([string]$index, $out)) } else { $out = $first }
            $j = Get-Content -LiteralPath $out -Raw | ConvertFrom-Json
            $fl = $j.device.features.FEATURE_LEVELS.list_all.MaxSupportedFeatureLevel
            $created = $j.device.created_at
            $ok = ($created -and ($fl -eq '12_1' -or $fl -eq '12_2'))
            Add-Result 'D3D12 FL 12_1' $ok "adapter $index, device created at $created, max feature level $fl"
        }
    } catch { Add-Result 'D3D12 FL 12_1' $false $_.Exception.Message }

    # Vulkan through the system loader, in a child process so that a failing ICD cannot take the installer down.
    $vk = Join-Path $here 'vk-check.ps1'
    $vkOut = (Invoke-Native powershell.exe @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $vk)).text.Trim()
    if ($vkOut -match '^SKIP') { Write-Host "   [skip] Vulkan               $vkOut" -ForegroundColor Yellow; Write-Log "   verify Vulkan: $vkOut" }
    else { Add-Result 'Vulkan' ($vkOut -match '(?m)^device 0x1002:0x13FE') $vkOut }

    $report = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); package_version = $(if ($script:Manifest) { $script:Manifest.version } else { $null }); results = $results }
    [IO.File]::WriteAllText((Join-Path $vdir ('verify-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.json')), ($report | ConvertTo-Json -Depth 5))
    return $results
}

if ($script:VerifyOnly) {
    $r = Invoke-Verify
    $bad = @($r | Where-Object { -not $_.pass })
    if (-not $DryRun) { Save-Phase $(if ($bad.Count) { 'verify-failed' } else { 'verified' }) }
    if ($bad.Count) { Write-Host "Verification: $($bad.Count) check(s) failed. See INSTALL.md, section 'If something fails'." -ForegroundColor Red; exit 3 }
    Write-Host 'Verification passed. The BC-250 runs on the amdgpu-wddm driver.' -ForegroundColor Green
    exit 0
}
if ($state.phase -in @('verified', 'verify-failed')) {
    Write-Host "This computer already has the release installed (phase $($state.phase)). Run verify.cmd to check it, or uninstall.cmd to remove it." -ForegroundColor White
    exit 0
}

# ---- phase 1: test signing -------------------------------------------------------------------------------------
function New-RestorePoint {
    Invoke-Change 'create a System Restore point "amdgpu-wddm before install" (only if System Protection is on)' {
        try { Checkpoint-Computer -Description 'amdgpu-wddm before install' -RestorePointType MODIFY_SETTINGS -ErrorAction Stop; Write-Info 'restore point created' }
        catch { Write-Warn2 "no restore point: $($_.Exception.Message)" }
    } | Out-Null
    Set-StateValue $state 'restore_point_attempted' (-not $script:DryRunMode)
}

if ($state.phase -eq 'new') {
    Write-Step 'Phase 1: save state, restore point, test signing'
    Set-StateValue $state 'previous_service' $script:DeviceService
    Set-StateValue $state 'previous_testsigning' $script:TestSigningConfigured
    Set-StateValue $state 'install_root' $InstallRoot
    Set-StateValue $state 'package_version' $script:Manifest.version
    New-RestorePoint
    if ($script:TestSigningActive) {
        Write-Info 'test signing is already active in this boot: no restart needed before the install'
        Set-StateValue $state 'testsigning_set_by_installer' $false
        Save-Phase 'testsigning-active'
    } else {
        Write-Host ''
        Write-Host '   Windows must run in TEST MODE to load this driver. Test mode lets Windows load drivers that are' -ForegroundColor White
        Write-Host '   signed with a test certificate, and shows "Test Mode" on the desktop. Uninstall can turn it off.' -ForegroundColor White
        $ok = Read-Confirmation -Question 'Turn on test signing (bcdedit /set testsigning on)?' -Answer $(if ($AcceptTestSigning) { 'YES' } else { $null })
        if (-not $ok) { Write-Host 'Stopped: test signing not confirmed. Nothing else was changed.' -ForegroundColor Yellow; if (-not $script:DryRunMode) { Save-Phase 'new' }; exit 4 }
        if ($script:BitLockerState -eq 'on') {
            $choice = $BitLocker
            if (-not $choice) {
                if (Read-Confirmation -Question 'BitLocker is on. Do you have the recovery key for this drive (it will be asked for after the restart)?' -Answer $null) { $choice = 'HaveKey' }
                elseif (Read-Confirmation -Question 'Suspend BitLocker for the next two restarts instead?' -Answer $null) { $choice = 'Suspend' }
            }
            if (-not $choice) { Write-Host 'Stopped: BitLocker not handled. Nothing else was changed.' -ForegroundColor Yellow; exit 4 }
            if ($choice -eq 'Suspend') {
                Invoke-Change "suspend BitLocker on $($env:SystemDrive) for two restarts" { Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 2 | Out-Null } | Out-Null
            }
            Set-StateValue $state 'bitlocker' $choice
        }
        Invoke-Change 'bcdedit /set {current} testsigning on' {
            $n = Invoke-Native bcdedit.exe @('/set', '{current}', 'testsigning', 'on')
            if ($n.code -ne 0) { throw "bcdedit failed: $($n.text)" }
        } | Out-Null
        Set-StateValue $state 'testsigning_set_by_installer' $true
        Save-Phase 'testsigning-pending'
        Set-ResumeAtLogon (Join-Path $package 'install.cmd')
        Request-Restart 'test signing takes effect at the next start.'
        if (-not $script:DryRunMode) { exit 0 }
        Write-Info '(dry run: phase 2 is shown as it would run after the restart)'
        Save-Phase 'testsigning-active'
    }
}
if ($state.phase -eq 'testsigning-pending') {
    if (-not $script:TestSigningActive) {
        Write-Host 'Test signing is set but not active yet: restart the computer, then run install.cmd again.' -ForegroundColor Yellow
        if ($script:SecureBoot -eq 'on') { Write-Host 'Secure Boot is on, so Windows ignores test signing. Turn it off in the BIOS setup.' -ForegroundColor Yellow }
        exit 5
    }
    Save-Phase 'testsigning-active'
}

# ---- phase 2: install ------------------------------------------------------------------------------------------
Write-Step 'Phase 2: install'
if (-not $script:DryRunMode -and -not $script:TestSigningActive) { Write-Fail 'test signing is not active'; exit 5 }

$cer = Join-Path $package 'payload\cert\amdgpu-wddm-release.cer'
$cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2 -ArgumentList $cer
Set-StateValue $state 'cert_thumbprint' $cert.Thumbprint
Invoke-Change "add the release test certificate $($cert.Thumbprint) to LocalMachine Root and TrustedPublisher" {
    foreach ($store in 'Root', 'TrustedPublisher') { Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null }
} | Out-Null

# Files. Each payload directory goes to the same name under the install root.
$dirs = @('d3d12', 'desktop', 'd3d11', 'vulkan', 'tools')
if (-not $NoControlApp -and (Test-Path -LiteralPath (Join-Path $package 'payload\control'))) { $dirs += 'control' }
foreach ($d in $dirs) {
    $src = Join-Path $package "payload\$d"
    Invoke-Change "copy payload\$d -> $InstallRoot\$d" {
        [void][IO.Directory]::CreateDirectory((Join-Path $InstallRoot $d))
        Copy-Item -Path (Join-Path $src '*') -Destination (Join-Path $InstallRoot $d) -Recurse -Force
    } | Out-Null
}
Invoke-Change "copy the installer's own scripts to $InstallRoot\installer (uninstall works without the package folder)" {
    [void][IO.Directory]::CreateDirectory((Join-Path $InstallRoot 'installer'))
    Copy-Item -Path (Join-Path $here '*') -Destination (Join-Path $InstallRoot 'installer') -Force
    Copy-Item -LiteralPath (Join-Path $package 'manifest.json') -Destination (Join-Path $InstallRoot 'manifest.json') -Force
    Copy-Item -LiteralPath (Join-Path $package 'uninstall.cmd') -Destination (Join-Path $InstallRoot 'uninstall.cmd') -Force
    Copy-Item -LiteralPath (Join-Path $package 'verify.cmd') -Destination (Join-Path $InstallRoot 'verify.cmd') -Force
} | Out-Null
$stub = Join-Path $env:windir 'System32\bc250umd.dll'
Set-StateValue $state 'stub_existed' (Test-Path -LiteralPath $stub)
Invoke-Change "copy payload\system32\bc250umd.dll -> $stub (the D3D9 slot of UserModeDriverName)" {
    Copy-Item -LiteralPath (Join-Path $package 'payload\system32\bc250umd.dll') -Destination $stub -Force
} | Out-Null
Set-StateValue $state 'firmware_dir_existed' (Test-Path -LiteralPath $script:FirmwareDir)
Set-StateValue $state 'bc250_dir_existed' (Test-Path -LiteralPath 'C:\BC250')
Invoke-Change "copy the 8 GPU firmware files (linux-firmware cyan_skillfish2_*.bin) -> $($script:FirmwareDir); C:\BC250 writable by administrators only" {
    [void][IO.Directory]::CreateDirectory($script:FirmwareDir)
    # C:\ lets every user create and change folders; the KMD loads this firmware, so only administrators may change it.
    $n = Invoke-Native icacls.exe @('C:\BC250', '/inheritance:r', '/grant:r', '*S-1-5-32-544:(OI)(CI)F', '*S-1-5-18:(OI)(CI)F', '*S-1-5-32-545:(OI)(CI)RX')
    if ($n.code -ne 0) { throw "icacls C:\BC250 failed: $($n.text)" }
    Copy-Item -Path (Join-Path $package 'payload\firmware\*.bin') -Destination $script:FirmwareDir -Force
} | Out-Null
Save-Phase 'files-copied'

# The driver package. The device starts once right away with the INF's closed gates (display only); the registry
# below opens them for the next start.
Invoke-Change 'pnputil /add-driver payload\kmd\bc250kmd.inf /install' {
    $n = Invoke-Native pnputil.exe @('/add-driver', (Join-Path $package 'payload\kmd\bc250kmd.inf'), '/install')
    Write-Log $n.text
    if ($n.code -ne 0 -and $n.code -ne 3010) { throw "pnputil failed ($($n.code)): $($n.text)" }
} | Out-Null
$instance = $null
if ($script:Device) { $instance = $script:Device.DeviceID }
$classKey = $null
if (-not $script:DryRunMode) {
    $svc = Get-DeviceServiceName -InstanceId $instance
    if ($svc -ne $script:ServiceName) { Write-Fail "the GPU is on '$svc' after pnputil, not on $($script:ServiceName)"; Save-Phase 'install-failed'; exit 6 }
    $classKey = Get-DeviceDriverKey -InstanceId $instance
    if (-not $classKey) { Write-Fail 'no software key for the GPU'; Save-Phase 'install-failed'; exit 6 }
    Set-StateValue $state 'class_key' $classKey
} else { $classKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\$($script:DisplayClassGuid)\<device's key after install>" }
Save-Phase 'driver-installed'

# Driver parameters: the registered lab configuration's gates, with the tester defaults for clocks.
$params = [ordered]@{
    EnableMmio = 1; EnableVram = 1; EnableVramWrite = 1; EnableGart = 1; EnablePsp = 1; EnableGfx = 1; EnableIh = 1
    EnableDcnWrite = 1; EnableVidPnFlip = 1; EnableGpuVa = 1; EnableGpuSubmit = 1; EnablePagingNode = 1; EnablePresentBlit = 1
    EnableFullWddm = 2                     # 2 = open at every start (1 opens one start only)
    EnableNativeSmu = 1; EnableNativePteCopy = 1; EnableHandleIdentityProbe = 1
    EnableGpuPresentBlit = 1; EnableCddDwmInterop = 1   # desktop composition on the GPU
    DpmMode = 1; DpmMaxMHz = $DpmMaxMHz    # load-driven clocks; thermal limits are the driver's own
    KeepLog = 0                            # no log files on the tester's disk
    UnconfirmedStarts = 0                  # the INF's own reset; the display-only start above already counted one
}
if ($CuMode) { $params['CuMode'] = $CuMode }
Invoke-Change ("set $($params.Count) DWORD values in $($script:ParametersKey): " + (($params.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')) {
    New-Item -Path $script:ParametersKey -Force | Out-Null
    foreach ($e in $params.GetEnumerator()) { New-ItemProperty -LiteralPath $script:ParametersKey -Name $e.Key -Value ([int]$e.Value) -PropertyType DWord -Force | Out-Null }
} | Out-Null

# Graphics registration in the GPU's software key: D3D9/10/11 slots, D3D12 slot, Vulkan.
$umd = @('bc250umd.dll', (Join-Path $InstallRoot 'desktop\bc250d3d_router.dll'), (Join-Path $InstallRoot 'desktop\bc250d3d_router.dll'), (Join-Path $InstallRoot 'd3d12\amdgpu_wddm_d3d12.dll'))
$icdJson = Join-Path $InstallRoot 'vulkan\radeon_icd.json'
Invoke-Change ("$classKey UserModeDriverName = " + ($umd -join ' | ') + "; VulkanDriverName = $icdJson") {
    New-ItemProperty -LiteralPath $classKey -Name UserModeDriverName -Value ([string[]]$umd) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath $classKey -Name VulkanDriverName -Value ([string[]]@($icdJson)) -PropertyType MultiString -Force | Out-Null
} | Out-Null
Invoke-Change "$($script:KhronosKey) '$icdJson' = 0 (system Vulkan ICD)" {
    New-Item -Path $script:KhronosKey -Force | Out-Null
    New-ItemProperty -LiteralPath $script:KhronosKey -Name $icdJson -Value 0 -PropertyType DWord -Force | Out-Null
} | Out-Null
Set-StateValue $state 'khronos_value' $icdJson

# Router policy (HKLM\SOFTWARE\amdgpu-wddm): DWM on the GPU, D3D11 applications on the CPU UMD unless allowed.
Invoke-Change "$($script:SoftwareKey)\DesktopRouter: CpuUmdPath, DwmForceCpu 0, RequireKmdSwitches 1" {
    $k = "$($script:SoftwareKey)\DesktopRouter"
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name CpuUmdPath -Value (Join-Path $InstallRoot 'desktop\bc250d3d.dll') -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name DwmForceCpu -Value 0 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name RequireKmdSwitches -Value 1 -PropertyType DWord -Force | Out-Null
} | Out-Null
Invoke-Change "$($script:SoftwareKey)\AppRouter: Mode allowlist, GpuUmdPath d3d11, Allow dxdiag.exe, Deny witcher3.exe" {
    $k = "$($script:SoftwareKey)\AppRouter"
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name Mode -Value 'allowlist' -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name GpuUmdPath -Value (Join-Path $InstallRoot 'd3d11\amdgpu_wddm_d3d11.dll') -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name Allow -Value ([string[]]@('dxdiag.exe')) -PropertyType MultiString -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name Deny -Value ([string[]]@('witcher3.exe')) -PropertyType MultiString -Force | Out-Null
} | Out-Null
$w3 = 'present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff,deferred-replay'
Invoke-Change "$($script:SoftwareKey)\D3D12\Applications\witcher3.exe Experiment = $w3" {
    $k = "$($script:SoftwareKey)\D3D12\Applications\witcher3.exe"
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name Experiment -Value $w3 -PropertyType String -Force | Out-Null
} | Out-Null
Invoke-Change "$($script:SoftwareKey)\Release: Version, InstallDir, InstallRoot, InstalledUtc" {
    $k = "$($script:SoftwareKey)\Release"
    New-Item -Path $k -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name Version -Value ([string]$script:Manifest.version) -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name InstallDir -Value $InstallRoot -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name InstallRoot -Value $InstallRoot -PropertyType String -Force | Out-Null
    New-ItemProperty -LiteralPath $k -Name InstalledUtc -Value ([DateTime]::UtcNow.ToString('o')) -PropertyType String -Force | Out-Null
} | Out-Null

# The start-confirm task: after every logon it confirms the boot's driver start, so the boot-loop guard does not
# fall back to Basic Display at the third start (see INSTALL.md, "The small blue window after logon").
$sc = Join-Path $InstallRoot 'tools\start-confirm.ps1'
Invoke-Change "scheduled task '$($script:TaskName)': at logon of an administrator, runs $sc (at most 2 minutes)" {
    $old = Get-ScheduledTask -TaskName $script:TaskName -ErrorAction SilentlyContinue
    if ($old) { Unregister-ScheduledTask -TaskName $script:TaskName -Confirm:$false }
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$sc`""
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    $principal = New-ScheduledTaskPrincipal -GroupId 'S-1-5-32-544' -RunLevel Highest
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 3) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $script:TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings | Out-Null
} | Out-Null

# The control application: a Start menu entry, nothing resident.
$controlExe = $null
if ($script:Manifest -and $script:Manifest.control_app_exe) { $controlExe = Join-Path $InstallRoot ('control\' + $script:Manifest.control_app_exe) }
if ($controlExe -and $dirs -contains 'control') {
    $lnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\amdgpu-wddm Control.lnk'
    Invoke-Change "Start menu shortcut $lnk -> $controlExe" {
        $sh = New-Object -ComObject WScript.Shell
        $s = $sh.CreateShortcut($lnk); $s.TargetPath = $controlExe; $s.WorkingDirectory = (Split-Path $controlExe); $s.Save()
    } | Out-Null
    Set-StateValue $state 'shortcut' $lnk
} else { Write-Info 'control application: not in this package (or -NoControlApp); skipped' }

if ($script:DryRunMode) {
    Write-Host ''
    Write-Host 'Dry run complete: every check ran, nothing was changed.' -ForegroundColor Green
    exit 0
}
Save-Phase 'installed'
Set-ResumeAtLogon (Join-Path $InstallRoot 'verify.cmd')
Write-Host ''
Write-Host 'Installation complete. After the restart, the installer verifies the driver by itself.' -ForegroundColor Green
Request-Restart 'the driver starts with its full configuration at the next start.'
exit 0
