#Requires -Version 5
# The board memory operation of the control application, run through the application's own elevated verb.
# Generic: it names no train and no package.
#
# The Board memory card of the control application asks for a confirmation, and then starts itself elevated
# with `--bc250-board-memory-action <8192|12288|restore> <token>`. The token is a SHA-256 over the board state
# that the card showed, and the elevated process refuses it when the state has changed since. An SSH session of
# the lab is elevated, so this script calls that verb directly, with a token that the application's own code
# computes: the script loads amdgpu_wddm_control.exe as an assembly and calls Kmd.BoardMemoryQuery and
# UmaSetting.ConfirmationToken. The click on the card and the confirmation dialog are the parts this does not
# cover. Their result is the same command line.
#
#   read      the state that the card shows: the active size, the size of the next start, the backup
#   set       read, then -Target (8192 or 12288) through the verb, then read again: the next start must be
#             -Target and a restart must be pending
#   restore   read, then Restore through the verb (the size that the backup holds), then read again
#   -Render   also render the application's pages as the interactive user (--smoke-render): the card is on the
#             graphics page, graphics-full.png
#
# set and restore write the board's memory block (extended CMOS). They refuse to start when the diagnostic stick
# would boot Linux at the next restart. The size changes at the next start of Windows, which this script does
# not do. Result lines:
#   board memory state: active <MiB> MiB, next start <MiB> MiB, previous <MiB> MiB, backup <b>, write <b>, reason <n>, pending <b>
#   board memory <set|restore>: exit <n>, next start <MiB> MiB, verified <b>
param(
    [ValidateSet('read', 'set', 'restore')][string]$Step = 'read',
    [ValidateSet('', '8192', '12288')][string]$Target = '',
    [string]$Out = 'C:\BC250\tmp\train-board-memory',
    [switch]$Render
)
$ErrorActionPreference = 'Continue'
$inst = [string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot).InstallRoot
$exe = Join-Path $inst 'control\amdgpu_wddm_control.exe'
$dll = Join-Path $inst 'control\bc250control.dll'
New-Item -ItemType Directory -Force $Out | Out-Null
"board memory $Step $Target start $([DateTime]::UtcNow.ToString('o'))"
foreach ($p in $exe, $dll) {
    if (-not (Test-Path -LiteralPath $p)) { "MISSING $p"; exit 2 }
    "$(Split-Path -Leaf $p) sha256 $((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0, 8))"
}
if ($Step -eq 'set' -and -not $Target) { 'set needs -Target 8192 or 12288'; exit 2 }

# The DLL first, by its full path: the application's P/Invoke names bc250control.dll alone, and in this
# process that name would be searched next to powershell.exe.
Add-Type -Namespace Bc250Train -Name Loader -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern IntPtr LoadLibraryW(string path);
'@
if ([Bc250Train.Loader]::LoadLibraryW($dll) -eq [IntPtr]::Zero) { "LoadLibrary $dll failed"; exit 2 }
$asm = [Reflection.Assembly]::LoadFrom($exe)
$kmdType = $asm.GetType('AmdgpuWddmControl.Kmd', $true)
$umaType = $asm.GetType('AmdgpuWddmControl.UmaSetting', $true)
function Call($type, [string]$name, [object[]]$arguments) { $type.GetMethod($name).Invoke($null, $arguments) }

function Read-State([string]$When) {
    $r = Call $kmdType 'BoardMemoryQuery' @()
    if (-not $r.Value) { "board memory state ($When): no answer, status 0x$('{0:X8}' -f $r.Status) $($r.Error)"; return $null }
    $s = $r.Value
    $active = [int]($s.ActiveBytes / 1MB)
    $pending = Call $umaType 'Pending' @(, $s)
    $visible = Call $umaType 'Visible' @(, $s)
    "board memory state ($When): active $active MiB, next start $($s.RequestedMiB) MiB, previous $($s.PreviousMiB) MiB, backup $($s.BackupAvailable), write $($s.WriteAllowed), reason $($s.Reason), pending $pending"
    "  supported $($s.Supported) visible $visible provider $($s.ProviderId) choices $($s.AllowedFirstMiB)/$($s.AllowedSecondMiB) needs restart $($s.NeedsRestart) result code $($s.ResultCode)"
    "  can set 8192 $(Call $umaType 'CanSet' @($s, [uint32]8192)), can set 12288 $(Call $umaType 'CanSet' @($s, [uint32]12288)), can restore $(Call $umaType 'CanRestore' @(, $s))"
    $script:state = $s
}

# The stick decides what a restart boots: bootx64.efi present means Linux. Only a removable FAT32 volume that
# holds efi\boot is looked at.
function Stick() {
    foreach ($v in @(Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.DriveType -eq 'Removable' -and $_.FileSystem -eq 'FAT32' -and $_.DriveLetter })) {
        $boot = "$($v.DriveLetter):\efi\boot"
        if (-not (Test-Path -LiteralPath $boot)) { continue }
        if (Test-Path -LiteralPath "$boot\bootx64.efi") { return 'linux' }
        if (Test-Path -LiteralPath "$boot\bootx64.off") { return 'windows' }
    }
    return 'no stick'
}

$script:state = $null
Read-State 'before'
$stick = Stick
"stick loader: $stick"

if ($Step -ne 'read') {
    if (-not $script:state) { "board memory ${Step}: no state, nothing written"; exit 1 }
    if ($stick -eq 'linux') { "board memory ${Step}: the next restart would boot Linux from the stick, nothing written"; exit 1 }
    $s = $script:state
    $wanted = if ($Step -eq 'restore') { [uint32]$s.PreviousMiB } else { [uint32]$Target }
    $allowed = if ($Step -eq 'restore') { Call $umaType 'CanRestore' @(, $s) } else { Call $umaType 'CanSet' @($s, $wanted) }
    if (-not $allowed) { "board memory ${Step}: the application would not offer this ($wanted MiB), nothing written"; exit 1 }
    $token = Call $umaType 'ConfirmationToken' @(, $s)
    "token $($token.Substring(0, 8))... for $wanted MiB"
    $verb = if ($Step -eq 'restore') { 'restore' } else { $Target }
    $p = Start-Process -FilePath $exe -ArgumentList @('--bc250-board-memory-action', $verb, $token) -PassThru -Wait
    $code = $p.ExitCode
    "verb exit $code (0 written and read back, 1 refused, 5 not elevated)"
    Read-State 'after'
    $after = $script:state
    $verified = [bool]($code -eq 0 -and $after -and $after.RequestedMiB -eq $wanted)
    "board memory ${Step}: exit $code, next start $(if ($after) { $after.RequestedMiB } else { '?' }) MiB, verified $verified"
}

if ($Render) {
    $user = (Get-CimInstance Win32_ComputerSystem).UserName
    $dir = Join-Path $Out ('render-' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
    New-Item -ItemType Directory -Force $dir | Out-Null
    & icacls.exe $dir /grant ("${user}:(OI)(CI)M") | Out-Null
    $cmd = Join-Path $dir 'run.cmd'
    $exit = Join-Path $dir 'exitcode.txt'
    @('@echo off', "`"$exe`" --smoke-render `"$dir`" 1.0 --lang en 1> `"$dir\stdout.txt`" 2>&1",
      "echo exit %ERRORLEVEL% > `"$exit`"") | Set-Content -LiteralPath $cmd -Encoding ASCII
    $task = 'Train control render'
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    $action = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument "--headless cmd.exe /c `"$cmd`""
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    Register-ScheduledTask -TaskName $task -Action $action -Principal $principal | Out-Null
    Start-ScheduledTask -TaskName $task
    $deadline = (Get-Date).AddSeconds(90)
    while ((Get-Date) -lt $deadline -and -not (Test-Path -LiteralPath $exit)) { Start-Sleep -Milliseconds 1000 }
    Start-Sleep -Seconds 2
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
    "render: $(if (Test-Path -LiteralPath $exit) { (Get-Content -LiteralPath $exit -Raw).Trim() } else { 'did not finish in 90 s' })"
    $png = Join-Path $dir 'graphics-full.png'
    if (Test-Path -LiteralPath $png) { Copy-Item -LiteralPath $png -Destination (Join-Path $Out 'graphics-full.png') -Force }
    "render page $png $(if (Test-Path -LiteralPath $png) { (Get-Item -LiteralPath $png).Length } else { 'ABSENT' }) bytes, latest copy $(Join-Path $Out 'graphics-full.png')"
    Get-Content (Join-Path $dir 'layout.txt') -ErrorAction SilentlyContinue | Select-Object -First 5 | ForEach-Object { '  layout: ' + $_ }
}
'board memory done'
if ($Step -ne 'read' -and -not $verified) { exit 1 }
exit 0
