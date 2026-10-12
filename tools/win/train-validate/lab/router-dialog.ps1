# BD-126 on hardware: the Windows firewall notification window takes the CPU UMD route and paints, and an
# Allow entry gives the same window back to the application GPU UMD (Windows PowerShell 5.1, SSH session).
#
#   router-dialog.ps1 -Phase default|allow|restore [-Wait 90] [-Port 50505]
#
# The repro needs no game. A program that listens on a network port from a path with no firewall rule makes
# Windows raise the notification window, and `svchost.exe` starts `PickerHost.exe`, the File Picker UI Host,
# to draw it. The window must be raised in the console session, so the listener runs there through a scheduled
# task: every call of a train arm arrives over ssh in session 0, where no window of this kind appears.
#
# Phases, in this order:
#   default  Mode gpu-default, Allow and Deny empty, RouteLogDirectory set. It starts the listener and reads
#            the route lines. The route of PickerHost.exe must be cpu with the reason app-dialog-host.
#   allow    PickerHost.exe in Allow, the same listener again. The route must be gpu with reason app-default.
#   restore  The AppRouter values go back to what the installer wrote, and the listener and its folder go.
#
# What this script cannot answer is whether the window paints its text and its two buttons. That is a picture,
# and the operator takes it with a half-resolution shot (tools/win/bc250mon/mon.py screenshot) while the window
# is up. The arm in arms.json holds that step. If the window stays empty on the CPU route as well, the entry
# buys nothing and comes out (driver/umd/router/README.md).
#
# The script writes only under C:\BC250\tmp and under HKLM\SOFTWARE\amdgpu-wddm\AppRouter, and the restore
# phase puts every value it touched back.
param([Parameter(Mandatory)][ValidateSet('default', 'allow', 'restore')][string]$Phase,
      [int]$Wait = 90, [int]$Port = 50505, [string]$TaskName = 'BC250 router dialog listener')
$ErrorActionPreference = 'Continue'
$bad = @()
$key = 'HKLM:\SOFTWARE\amdgpu-wddm\AppRouter'
$work = 'C:\BC250\tmp\router-dialog'
$routeDir = Join-Path $work 'route-log'
$listenerDir = Join-Path $work 'listener'
$listener = Join-Path $listenerDir 'bc250listener.exe'
$saved = Join-Path $work 'approuter-before.json'

function Remove-Listener {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    }
    Get-Process -Name 'bc250listener' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
}

function Get-ConsoleUser {
    $explorer = Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $explorer) { throw 'no explorer.exe: no console session is logged on, so no window can be raised' }
    $owner = Invoke-CimMethod -InputObject $explorer -MethodName GetOwner
    return '{0}\{1}' -f $owner.Domain, $owner.User
}

function Start-Listener {
    # A fresh copy of powershell.exe under a name of its own is a program Windows has no firewall rule for, so
    # the notification window is raised for this image and for no program of the operating system.
    [void][IO.Directory]::CreateDirectory($listenerDir)
    if (-not (Test-Path -LiteralPath $listener)) {
        Copy-Item -LiteralPath (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Destination $listener
    }
    Remove-Listener
    $script = Join-Path $work 'listen.ps1'
    @(
        '$l = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, ' + $Port + ')',
        '$l.Start()',
        'Start-Sleep -Seconds ' + [Math]::Max(30, $Wait),
        '$l.Stop()'
    ) | Set-Content -LiteralPath $script -Encoding ASCII
    $user = Get-ConsoleUser
    "console user $user"
    $action = New-ScheduledTaskAction -Execute $listener `
        -Argument ('-NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $script) -WorkingDirectory $listenerDir
    $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Limited
    $settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Seconds ($Wait + 60)) `
        -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $TaskName -Action $action -Principal $principal -Settings $settings | Out-Null
    Start-ScheduledTask -TaskName $TaskName
    "listener started, port $Port, image $listener"
}

function Show-RouteLines {
    $lines = @()
    if (Test-Path -LiteralPath $routeDir) {
        foreach ($f in Get-ChildItem -LiteralPath $routeDir -File -Filter 'route-*.log' -ErrorAction SilentlyContinue) {
            foreach ($l in Get-Content -LiteralPath $f.FullName -ErrorAction SilentlyContinue) { $lines += $l }
        }
    }
    "route lines $($lines.Count)"
    foreach ($l in $lines) { '  ' + $l }
    return $lines
}

try {
    "utc $([DateTime]::UtcNow.ToString('o')) phase $Phase"
    [void][IO.Directory]::CreateDirectory($work)
    [void][IO.Directory]::CreateDirectory($routeDir)
    $before = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
    "approuter before: Mode=$($before.Mode) Allow=[$(@($before.Allow) -join ',')] Deny=[$(@($before.Deny) -join ',')] RouteLogDirectory=$($before.RouteLogDirectory) GpuUmdPath=$($before.GpuUmdPath)"

    if ($Phase -eq 'restore') {
        Remove-Listener
        if (Test-Path -LiteralPath $saved) {
            $old = Get-Content -LiteralPath $saved -Raw | ConvertFrom-Json
            Set-ItemProperty -Path $key -Name 'Mode' -Value ([string]$old.Mode)
            New-ItemProperty -Path $key -Name 'Allow' -PropertyType MultiString -Value ([string[]]@($old.Allow)) -Force | Out-Null
            New-ItemProperty -Path $key -Name 'Deny' -PropertyType MultiString -Value ([string[]]@($old.Deny)) -Force | Out-Null
            if ([string]::IsNullOrEmpty([string]$old.RouteLogDirectory)) {
                Remove-ItemProperty -Path $key -Name 'RouteLogDirectory' -ErrorAction SilentlyContinue
            } else {
                Set-ItemProperty -Path $key -Name 'RouteLogDirectory' -Value ([string]$old.RouteLogDirectory)
            }
            'approuter restored from the saved record'
        } else { $bad += "no saved record at ${saved}: restore the AppRouter values by hand" }
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
        $after = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        "approuter after: Mode=$($after.Mode) Allow=[$(@($after.Allow) -join ',')] Deny=[$(@($after.Deny) -join ',')] RouteLogDirectory=$($after.RouteLogDirectory)"
    }
    else {
        if ($Phase -eq 'default' -and -not (Test-Path -LiteralPath $saved)) {
            @{ Mode = [string]$before.Mode; Allow = @($before.Allow); Deny = @($before.Deny)
               RouteLogDirectory = [string]$before.RouteLogDirectory } |
                ConvertTo-Json | Set-Content -LiteralPath $saved -Encoding UTF8
            "approuter record saved to $saved"
        }
        Get-ChildItem -LiteralPath $routeDir -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
        Set-ItemProperty -Path $key -Name 'Mode' -Value 'gpu-default'
        Set-ItemProperty -Path $key -Name 'RouteLogDirectory' -Value $routeDir
        New-ItemProperty -Path $key -Name 'Deny' -PropertyType MultiString -Value ([string[]]@()) -Force | Out-Null
        $allow = if ($Phase -eq 'allow') { [string[]]@('PickerHost.exe') } else { [string[]]@() }
        New-ItemProperty -Path $key -Name 'Allow' -PropertyType MultiString -Value $allow -Force | Out-Null
        "approuter set: Mode=gpu-default Allow=[$($allow -join ',')] Deny=[] RouteLogDirectory=$routeDir"

        Start-Listener
        # The window is raised a second or two after the listener binds. The route line is written when the
        # dialog host opens the adapter, so the wait is for the line and not for a fixed time.
        $deadline = [DateTime]::UtcNow.AddSeconds($Wait)
        $picker = @()
        do {
            Start-Sleep -Seconds 5
            $picker = @(Show-RouteLines | Where-Object { $_ -match 'exe=PickerHost\.exe' })
        } while ($picker.Count -eq 0 -and [DateTime]::UtcNow -lt $deadline)

        $pickerProc = Get-Process -Name 'PickerHost' -ErrorAction SilentlyContinue
        "pickerhost running: $([bool]$pickerProc)"
        if ($picker.Count -eq 0) {
            $bad += 'no route line for PickerHost.exe: the notification window was not raised, or the route log is not written'
        } else {
            foreach ($l in $picker) { "picker route: $l" }
            $last = $picker[-1]
            if ($Phase -eq 'default') {
                if ($last -notmatch 'route=cpu') { $bad += "the default route of PickerHost.exe is not cpu: $last" }
                if ($last -notmatch 'reason=app-dialog-host') { $bad += "the reason is not app-dialog-host: $last" }
            } else {
                if ($last -notmatch 'route=gpu') { $bad += "with PickerHost.exe in Allow the route is not gpu: $last" }
                if ($last -notmatch 'reason=app-default') { $bad += "with PickerHost.exe in Allow the reason is not app-default: $last" }
            }
            if ($last -notmatch 'entry=OpenAdapter10_2') { "note: the entry of this call is not OpenAdapter10_2: $last" }
        }
        'take the half-resolution shot now: the window is up for the rest of the listener bound'
    }
}
catch { $bad += "probe error: $($_.Exception.Message)" }
finally { if ($Phase -ne 'restore') { } else { Remove-Listener } }

if ($bad.Count) {
    foreach ($b in $bad) { "problem: $b" }
    "router dialog arm: FAIL, $($bad.Count) problem(s)"
    exit 1
}
'router dialog arm: ok'
exit 0
