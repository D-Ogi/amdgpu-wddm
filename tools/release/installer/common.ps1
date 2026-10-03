# Shared functions of the tester installer (install.ps1, uninstall.ps1). Windows PowerShell 5.1 syntax only: the
# tester runs the copy of powershell.exe that ships with Windows 11, so no ternary, no '??', no '&&'.
#
# Every change to the system goes through Invoke-Change. In a dry run it prints what it would do and does nothing,
# so the dry run executes the same code path as the real run, checks included.

$script:ReleaseName      = 'amdgpu-wddm'
$script:ServiceName      = 'bc250kmd'
$script:HardwareIdPrefix = 'PCI\VEN_1002&DEV_13FE'          # bc250kmd.inf [Models.NTamd64]
$script:DisplayClassGuid = '{4d36e968-e325-11ce-bfc1-08002be10318}'
$script:SoftwareKey      = 'HKLM:\SOFTWARE\amdgpu-wddm'
$script:ParametersKey    = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$script:KhronosKey       = 'HKLM:\SOFTWARE\Khronos\Vulkan\Drivers'
$script:RunOnceKey       = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce'
$script:RunOnceName      = 'amdgpu-wddm-installer'
$script:TaskName         = 'amdgpu-wddm start confirm'
$script:FirmwareDir      = 'C:\BC250\firmware'                 # compiled into the KMD (psp.c BC250_PSP_FIRMWARE_DIR)
$script:StateDir         = Join-Path $env:ProgramData 'amdgpu-wddm\installer'
$script:StatePath        = Join-Path $script:StateDir 'state.json'
$script:DryRunMode       = $false
$script:LogPath          = $null
$script:CurrentStep      = '(before the first change)'
$script:ScheduleOldCopies = $true                              # host tests set $false: no MoveFileEx on the test PC

function Write-Step([string]$Text)  { Write-Host ''; Write-Host "== $Text" -ForegroundColor Cyan; Write-Log "== $Text" }
function Write-Info([string]$Text)  { Write-Host "   $Text"; Write-Log "   $Text" }
function Write-Warn2([string]$Text) { Write-Host "   WARNING: $Text" -ForegroundColor Yellow; Write-Log "   WARNING: $Text" }
function Write-Fail([string]$Text)  { Write-Host "   FAIL: $Text" -ForegroundColor Red; Write-Log "   FAIL: $Text" }
function Write-Log([string]$Text) {
    if (-not $script:LogPath) { return }
    try { [IO.File]::AppendAllText($script:LogPath, ([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ') + ' ' + $Text + "`r`n")) } catch { }
}

# One change to the system. $Action runs only in a real run; a dry run prints the description.
function Invoke-Change {
    param([Parameter(Mandatory)][string]$Description, [Parameter(Mandatory)][scriptblock]$Action)
    $script:CurrentStep = $Description
    if ($script:DryRunMode) { Write-Host "   [dry run] would: $Description" -ForegroundColor DarkYellow; return $null }
    Write-Info "doing: $Description"
    return (& $Action)
}
# The message for a step that threw: which step, why, and that a re-run with the same package finishes the job.
function Write-StepFailure($ErrorRecord) {
    Write-Fail "stopped at step: $($script:CurrentStep)"
    Write-Fail "$($ErrorRecord.Exception.Message) $($ErrorRecord.InvocationInfo.PositionMessage)"
    Write-Host ''
    Write-Host 'Nothing after this step ran. The steps before it are done and stay done.' -ForegroundColor Yellow
    Write-Host 'Fix the cause if the message names one, then run install.cmd again from the same package folder:' -ForegroundColor Yellow
    Write-Host 'it skips what is already in place and continues from here.' -ForegroundColor Yellow
    if ($script:LogPath) { Write-Host "Log: $($script:LogPath)" -ForegroundColor Yellow }
}

# A native program, its stdout and stderr as one text, and its exit code. Windows PowerShell 5.1 turns a stderr line
# of a native program into a terminating error under ErrorActionPreference Stop; here it stays text.
function Invoke-Native {
    param([Parameter(Mandatory)][string]$File, [string[]]$Arguments = @())
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $text = (& $File @Arguments 2>&1 | ForEach-Object { [string]$_ }) -join "`n" } finally { $ErrorActionPreference = $old }
    return @{ text = $text; code = $LASTEXITCODE }
}

# Restarts the calling script elevated (UAC prompt) and ends this process with exit code 10. Started from a .cmd
# launcher (AMDGPU_WDDM_LAUNCHER = its full path), the launcher itself is elevated, so the elevated run keeps the
# launcher's console and its final pause: one result window. The launcher skips its own pause on exit code 10, so
# the unelevated window closes at once. Never called in a dry run.
function Invoke-SelfElevation {
    param([Parameter(Mandatory)][string]$ScriptPath, [Parameter(Mandatory)]$Bound)
    if ($script:DryRunMode) { throw 'internal error: elevation requested in a dry run' }
    $args2 = @()
    # The launcher adds its own fixed arguments again (verify.cmd: -Verify); pass only the others.
    $fixed = @(([string]$env:AMDGPU_WDDM_LAUNCHER_FIXED) -split ',' | Where-Object { $_ })
    foreach ($k in $Bound.Keys) {
        if ($env:AMDGPU_WDDM_LAUNCHER -and $fixed -contains $k) { continue }
        $v = $Bound[$k]
        if ($v -is [System.Management.Automation.SwitchParameter]) { if ($v) { $args2 += "-$k" } }
        else { $args2 += "-$k"; $args2 += ('"' + [string]$v + '"') }
    }
    Write-Host 'Administrator rights are needed: Windows will ask for them now.'
    $launcher = $env:AMDGPU_WDDM_LAUNCHER
    if ($launcher -and (Test-Path -LiteralPath $launcher)) {
        if ($args2.Count) { Start-Process -FilePath $launcher -ArgumentList $args2 -Verb RunAs | Out-Null }
        else { Start-Process -FilePath $launcher -Verb RunAs | Out-Null }
    } else {
        Start-Process -FilePath powershell.exe -ArgumentList (@('-NoProfile', '-ExecutionPolicy', 'Bypass', '-NoExit', '-File', ('"' + $ScriptPath + '"')) + $args2) -Verb RunAs | Out-Null
    }
    exit 10
}

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    return (New-Object Security.Principal.WindowsPrincipal $id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Asks the tester. -Answer (unattended) wins; a dry run never asks and answers 'no'.
function Read-Confirmation {
    param([Parameter(Mandatory)][string]$Question, [string]$Expect = 'YES', [string]$Answer)
    if ($Answer) { Write-Info "$Question -> $Answer (from the command line)"; return ($Answer -ceq $Expect) }
    # A dry run shows the path a YES takes, so it answers YES without asking.
    if ($script:DryRunMode) { Write-Host "   [dry run] would ask: $Question (type $Expect); continuing as if answered $Expect" -ForegroundColor DarkYellow; return $true }
    $reply = Read-Host "   $Question Type $Expect to continue"
    Write-Log "   asked: $Question -> '$reply'"
    return ($reply -ceq $Expect)
}

# ---------------------------------------------------------------------------------------------------------------
# State kept between the phases (and for uninstall): %ProgramData%\amdgpu-wddm\installer\state.json.
function Read-InstallState {
    if (-not (Test-Path -LiteralPath $script:StatePath)) { return $null }
    return (Get-Content -LiteralPath $script:StatePath -Raw | ConvertFrom-Json)
}
function Save-InstallState($State) {
    if ($script:DryRunMode) { return }
    [void][IO.Directory]::CreateDirectory($script:StateDir)
    [IO.File]::WriteAllText($script:StatePath, ($State | ConvertTo-Json -Depth 6))
}
function Set-StateValue($State, [string]$Name, $Value) {
    if ($State.PSObject.Properties[$Name]) { $State.$Name = $Value } else { $State | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
}
# A fact about the computer before the install (did this file exist?): the first run records it, a re-run over a
# partial install keeps it, because by then the installer itself made the file exist.
function Set-StateValueOnce($State, [string]$Name, $Value) {
    if ($null -eq $State.PSObject.Properties[$Name]) { $State | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
    return $State.$Name
}

# ---------------------------------------------------------------------------------------------------------------
# SHA256 through .NET: works whatever modules the host process can load.
function Get-Sha256([string]$Path) {
    $s = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($s)) -replace '-', '') } finally { $s.Dispose() }
}
# Package integrity: manifest.json lists every file of the package with its SHA256 (build-release.ps1 writes it).
function Test-PackageManifest {
    param([Parameter(Mandatory)][string]$PackageRoot)
    $path = Join-Path $PackageRoot 'manifest.json'
    if (-not (Test-Path -LiteralPath $path)) { return @{ ok = $false; detail = 'manifest.json missing' } }
    $m = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    $bad = @()
    $count = 0
    foreach ($f in @($m.files)) {
        $count++
        $p = Join-Path $PackageRoot ($f.path -replace '/', '\')
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { $bad += "missing $($f.path)"; continue }
        if ((Get-Sha256 $p) -ne $f.sha256) { $bad += "changed $($f.path)" }
    }
    if ($bad.Count) { return @{ ok = $false; detail = ($bad -join '; '); manifest = $m } }
    return @{ ok = $true; detail = "$count files match manifest.json (version $($m.version))"; manifest = $m }
}

# ---------------------------------------------------------------------------------------------------------------
# System facts. Each returns data; the preflight decides.
function Get-Bc250Device {
    # Present devices only, matched on the hardware ID the INF matches.
    $all = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.DeviceID -like ($script:HardwareIdPrefix + '*') })
    return $all
}
function Get-TestSigningActive {
    # The options the running boot was started with; readable without elevation.
    $o = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control' -Name SystemStartOptions -ErrorAction SilentlyContinue).SystemStartOptions
    return ([string]$o -match 'TESTSIGNING')
}
function Get-TestSigningConfigured {
    # What the next boot will use; needs elevation. $null when unknown.
    if (-not (Test-IsAdmin)) { return $null }
    $n = Invoke-Native bcdedit.exe @('/enum', '{current}')
    if ($n.code -ne 0) { return $null }
    return ($n.text -match '(?im)^\s*testsigning\s+Yes\s*$')
}
function Get-SecureBootState {
    # 'on', 'off', 'legacy-bios' or 'unknown'
    try { if (Confirm-SecureBootUEFI -ErrorAction Stop) { return 'on' } else { return 'off' } }
    catch [System.PlatformNotSupportedException] { return 'legacy-bios' }
    catch {
        $v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\SecureBoot\State' -Name UEFISecureBootEnabled -ErrorAction SilentlyContinue).UEFISecureBootEnabled
        if ($v -eq 1) { return 'on' }
        if ($v -eq 0) { return 'off' }
        return 'unknown'
    }
}
function Get-BitLockerState {
    # 'off', 'on', 'suspended' or 'unknown'
    if (-not (Test-IsAdmin)) { return 'unknown' }
    try {
        $v = Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop
        if ([string]$v.ProtectionStatus -eq 'On') { return 'on' }
        if ([string]$v.VolumeStatus -eq 'FullyDecrypted') { return 'off' }
        return 'suspended'
    } catch {
        $out = (Invoke-Native manage-bde.exe @('-status', $env:SystemDrive)).text
        if ($out -match '(?im)Protection Status:\s+Protection On') { return 'on' }
        if ($out -match '(?im)Protection Status:\s+Protection Off') { return 'off' }
        return 'unknown'
    }
}
function Get-MemoryIntegrityState {
    $v = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -Name Enabled -ErrorAction SilentlyContinue).Enabled
    if ($v -eq 1) { return 'on' }
    return 'off'
}
function Get-VcRuntimeMissing {
    $missing = @()
    foreach ($f in 'vcruntime140.dll', 'vcruntime140_1.dll', 'msvcp140.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $env:windir "System32\$f"))) { $missing += $f }
    }
    return $missing
}
function Get-DeviceDriverKey {
    # The device's software (class) key, e.g. HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-...}\0001
    param([Parameter(Mandatory)][string]$InstanceId)
    $p = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Driver' -ErrorAction SilentlyContinue
    if (-not $p -or -not $p.Data) { return $null }
    return ('HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + [string]$p.Data)
}
function Get-DeviceServiceName {
    param([Parameter(Mandatory)][string]$InstanceId)
    $p = Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName 'DEVPKEY_Device_Service' -ErrorAction SilentlyContinue
    if ($p) { return [string]$p.Data }
    return $null
}
# A lab installation (the project's development unit) carries files under C:\BC250\m1x. The release installer does
# not merge into one.
function Test-LabInstallPresent {
    foreach ($d in 'C:\BC250\m15', 'C:\BC250\m14', 'C:\BC250\m10') { if (Test-Path -LiteralPath $d) { return $true } }
    return $false
}

# Our driver packages in the driver store (pnputil /enum-drivers), by original name.
function Get-OurDriverPackages {
    $out = (Invoke-Native pnputil.exe @('/enum-drivers')).text
    $blocks = $out -split "(\r?\n){2,}"
    $r = @()
    foreach ($b in $blocks) {
        if ($b -match '(?im)^\s*Original Name:\s*bc250kmd\.inf\s*$' -and $b -match '(?im)^\s*Published Name:\s*(oem\d+\.inf)\s*$') { $r += $Matches[1] }
    }
    return $r
}

# MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT): delete a file that a running process (DWM) still has mapped.
function Remove-FileAtReboot([string]$Path) {
    if (-not ('AmdgpuWddmInstaller.Native' -as [type])) {
        Add-Type -Namespace AmdgpuWddmInstaller -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string existing, string replacement, int flags);
'@
    }
    return [AmdgpuWddmInstaller.Native]::MoveFileEx($Path, $null, 4)
}
# Installs one file so that a re-run with the same package always finishes:
#  - target with the same SHA256: nothing to do ("already current");
#  - target absent or writable: copied;
#  - target in use (a DLL that DWM or an application has loaded cannot be overwritten, but it can be renamed):
#    renamed to <name>.old-<utc>, the new file copied in, the old one scheduled for deletion at the next restart
#    (needs administrator rights; otherwise, and as a second chance, verify removes *.old-* files).
# Returns 'current', 'copied' or 'replaced-in-use'.
function Copy-FileSafe {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination, [bool]$ScheduleOld = $script:ScheduleOldCopies)
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        if ((Get-Sha256 $Destination) -eq (Get-Sha256 $Source)) { Write-Log "   already current: $Destination"; return 'current' }
        try { Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop; Write-Log "   copied: $Destination"; return 'copied' }
        catch {
            Write-Log "   cannot overwrite $Destination ($($_.Exception.Message)): replacing by rename"
            $old = $Destination + '.old-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')
            [IO.File]::Move($Destination, $old)
            try { Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop }
            catch { [IO.File]::Move($old, $Destination); throw }
            $scheduled = $false
            if ($ScheduleOld) { try { $scheduled = Remove-FileAtReboot $old } catch { $scheduled = $false } }
            Write-Info "in use, replaced: $Destination (old copy $(Split-Path $old -Leaf) $(if ($scheduled) { 'is deleted at the next restart' } else { 'is deleted by verify' }))"
            return 'replaced-in-use'
        }
    }
    [void][IO.Directory]::CreateDirectory((Split-Path $Destination))
    Copy-Item -LiteralPath $Source -Destination $Destination -Force -ErrorAction Stop
    Write-Log "   copied: $Destination"
    return 'copied'
}
# Every file of a directory tree, through Copy-FileSafe.
function Copy-TreeSafe {
    param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Destination)
    $n = @{ current = 0; copied = 0; 'replaced-in-use' = 0 }
    foreach ($f in Get-ChildItem -LiteralPath $Source -Recurse -File) {
        $rel = $f.FullName.Substring($Source.TrimEnd('\').Length + 1)
        $n[(Copy-FileSafe -Source $f.FullName -Destination (Join-Path $Destination $rel))]++
    }
    Write-Info ("{0}: {1} copied, {2} already current, {3} replaced in use" -f $Destination, $n.copied, $n.current, $n['replaced-in-use'])
    return $n
}
# Old copies left by Copy-FileSafe (<name>.old-<utc>). Verify and uninstall call it; a copy still in use stays for
# the deletion scheduled at restart. Returns the number removed.
function Remove-OldCopies([string]$Directory, [string]$Filter = '*.old-*', [switch]$Recurse) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return 0 }
    $n = 0
    foreach ($i in @(Get-ChildItem -LiteralPath $Directory -File -Filter $Filter -Recurse:$Recurse -ErrorAction SilentlyContinue)) {
        if ($i.Name -notmatch '\.old-\d{8}T\d{6,9}Z$') { continue }
        try { Remove-Item -LiteralPath $i.FullName -Force -ErrorAction Stop; $n++; Write-Log "   removed old copy $($i.FullName)" } catch { }
    }
    return $n
}

function Remove-PathOrSchedule([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $files = @()
    if (Test-Path -LiteralPath $Path -PathType Container) { $files = @(Get-ChildItem -LiteralPath $Path -Recurse -File -Force | ForEach-Object FullName) } else { $files = @($Path) }
    $pending = 0
    foreach ($f in $files) {
        try { Remove-Item -LiteralPath $f -Force -ErrorAction Stop }
        catch { if (Remove-FileAtReboot $f) { $pending++ } else { Write-Warn2 "could not remove or schedule $f" } }
    }
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $dirs = @(Get-ChildItem -LiteralPath $Path -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending | ForEach-Object FullName) + @($Path)
        foreach ($d in $dirs) {
            try { Remove-Item -LiteralPath $d -Force -ErrorAction Stop } catch { [void](Remove-FileAtReboot $d) }
        }
    }
    if ($pending) { Write-Info "$pending file(s) in use: removal scheduled for the next restart" }
}
