# Lab probe of the logon images' GPU route (docs/design/logon-gpu-route.md). Windows PowerShell 5.1, run elevated
# over SSH (session 0). No interactive input. Every action is bounded and writes one text report to -Out.
#
#   snapshot        state only: kernel driver start state, AppRouter values, the latch stores, the processes
#                   LogonUI/LockApp/consent/CredentialUIBroker/winlogon/dwm with their D3D modules, and the route
#                   log lines of the logon images
#   lock            lock the console session (a one-shot task in that session runs LockWorkStation), then watch
#   unlock          reconnect the console session from Local System (tscon), then watch: LogonUI.exe must end and
#                   its latch marker must be gone (a clean exit)
#   kill-logonui    end LogonUI.exe with TerminateProcess (the fault model of the latch), then watch whether
#                   winlogon.exe starts it again and on which UMD
#   uac             start an elevation request in the console session, so that consent.exe opens, then watch
#   kill-consent    end consent.exe with TerminateProcess (the elevation request fails), then watch
#   routelog-on     AppRouter RouteLogDirectory = <Out>\routelogs (a lab-only change; routelog-off removes it)
#   routelog-off
#   protected-off   AppRouter ProtectedGpu = 0 (the bisect switch); protected-on removes the value (absent = on)
#   protected-on
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File logon-gpu-probe.ps1 -Action snapshot -Out C:\BC250\tmp\logon-gpu
#   [-Seconds 60]   watch length after an action, 5 to 170 (default 60); one sample every 5 s
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('snapshot', 'lock', 'unlock', 'kill-logonui', 'uac', 'kill-consent', 'routelog-on', 'routelog-off',
                 'protected-off', 'protected-on')]
    [string]$Action,
    [Parameter(Mandatory = $true)][string]$Out,
    [ValidateRange(5, 170)][int]$Seconds = 60
)
$ErrorActionPreference = 'Continue'
$images = @('LogonUI', 'LockApp', 'consent', 'CredentialUIBroker', 'winlogon', 'dwm')
$moduleRx = '^(bc250d3d.*|amdgpu_wddm.*|d3d11|d3d10.*|dxgi|d3d12|d3d12core|vulkan-1|dcomp)\.dll$'
$appRouter = 'HKLM:\SOFTWARE\amdgpu-wddm\AppRouter'
$taskPrefix = 'logon-gpu-probe-'
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$report = Join-Path $Out ("$stamp-$Action.txt")
function Say([string]$text) {
    $line = (Get-Date).ToUniversalTime().ToString('HH:mm:ss') + 'Z ' + $text
    Add-Content -LiteralPath $report -Value $line
    Write-Output $line
}

function Show-StartState {
    # The values the router reads for the start and latch gates, wherever the driver state key keeps them.
    $root = 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd'
    $want = @('UnconfirmedStarts', 'Confirmed', 'AdapterRestarts', 'InteropLastState', 'InteropClosedReason')
    $keys = @(Get-Item -LiteralPath $root -ErrorAction SilentlyContinue) +
            @(Get-ChildItem -LiteralPath $root -Recurse -ErrorAction SilentlyContinue)
    foreach ($k in $keys) {
        if (-not $k) { continue }
        foreach ($n in $k.GetValueNames()) {
            if ($want -contains $n) { Say ('state ' + ($k.Name -replace '^HKEY_LOCAL_MACHINE', 'HKLM') + ' ' + $n + '=' + $k.GetValue($n)) }
        }
    }
}

function Show-AppRouter {
    $k = Get-Item -LiteralPath $appRouter -ErrorAction SilentlyContinue
    if (-not $k) { Say 'AppRouter key absent'; return }
    foreach ($n in 'Mode', 'ProtectedGpu', 'RouteLogDirectory', 'Deny', 'Allow') {
        if ($k.GetValueNames() -contains $n) { Say ("AppRouter $n=" + (@($k.GetValue($n)) -join ',') + ' (' + $k.GetValueKind($n) + ')') }
        else { Say "AppRouter $n absent" }
    }
}

function Show-Latch {
    $stores = @('HKLM:\SOFTWARE\amdgpu-wddm\ProtectedLatch')
    foreach ($u in @(Get-ChildItem -LiteralPath 'Registry::HKEY_USERS' -ErrorAction SilentlyContinue)) {
        $stores += 'Registry::' + $u.Name + '\Software\amdgpu-wddm\ProtectedLatch'
    }
    foreach ($s in $stores) {
        $k = Get-Item -LiteralPath $s -ErrorAction SilentlyContinue
        if (-not $k) { continue }
        $names = @($k.GetValueNames())
        if (-not $names.Count) { Say "latch $s (empty)"; continue }
        foreach ($n in $names) { Say ("latch $s $n=" + $k.GetValue($n)) }
    }
}

# The second module reader, in-process: K32EnumProcessModulesEx (LIST_MODULES_ALL, both bitnesses) and
# K32GetModuleFileNameExW, the method of bc250mon (src/GraphicsApiProvider.cs). The watch loop samples every 5 s, so
# it must not start a process for each sample (the spawn-loop gate, BD-051). Add-Type compiles once, here.
$modReader = $true
$modReaderError = ''
$modReaderTold = $false
try {
    Add-Type -Namespace LogonProbe -Name Mods -ErrorAction Stop -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
[DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
[DllImport("kernel32.dll", EntryPoint = "K32EnumProcessModulesEx", SetLastError = true)]
static extern bool EnumProcessModulesEx(IntPtr h, [Out] IntPtr[] modules, int bytes, out int needed, uint filter);
[DllImport("kernel32.dll", EntryPoint = "K32GetModuleFileNameExW", CharSet = CharSet.Unicode)]
static extern int GetModuleFileNameExW(IntPtr h, IntPtr module, System.Text.StringBuilder name, int size);
// The full paths of the modules of a process, or the Win32 error of the step that failed.
public static string[] List(int pid, out string error) {
    error = "";
    IntPtr h = OpenProcess(0x1000 | 0x0010, false, pid); // PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ
    if (h == IntPtr.Zero) { error = "OpenProcess " + Marshal.GetLastWin32Error(); return null; }
    try {
        IntPtr[] mods = new IntPtr[1024];
        int needed;
        if (!EnumProcessModulesEx(h, mods, mods.Length * IntPtr.Size, out needed, 3)) {
            error = "EnumProcessModulesEx " + Marshal.GetLastWin32Error(); return null;
        }
        int count = Math.Min(needed / IntPtr.Size, mods.Length);
        string[] paths = new string[count];
        System.Text.StringBuilder b = new System.Text.StringBuilder(1024);
        for (int i = 0; i < count; i++) {
            b.Length = 0;
            int n = GetModuleFileNameExW(h, mods[i], b, b.Capacity);
            if (n >= b.Capacity) {
                // A return equal to the capacity means a cut path: retry once with the extended-length limit.
                b.Capacity = 32768;
                b.Length = 0;
                n = GetModuleFileNameExW(h, mods[i], b, b.Capacity);
                if (n >= b.Capacity) { n = 0; }
            }
            paths[i] = n > 0 ? b.ToString() : "?";
        }
        return paths;
    } finally { CloseHandle(h); }
}
'@
} catch { $modReader = $false; $modReaderError = $_.Exception.Message }

function Read-Modules([int]$id) {
    # The second reader as module objects with the names Show-Processes uses, or $null with the reason in the report.
    # Say goes to the host here: its output in the pipeline would become part of the return value.
    if (-not $modReader) {
        # Say the reason once, on the first sample that sees it; the later samples keep the short line.
        if (-not $script:modReaderTold) { $script:modReaderTold = $true; Say ('  modules: no in-process reader: ' + $script:modReaderError) | Out-Host }
        else { Say '  modules: no in-process reader (Add-Type failed)' | Out-Host }
        return $null
    }
    $err = ''
    $paths = [LogonProbe.Mods]::List($id, [ref]$err)
    if ($null -eq $paths) { Say ("  modules (in-process reader): " + $err) | Out-Host; return $null }
    return @($paths | ForEach-Object { [pscustomobject]@{ ModuleName = [IO.Path]::GetFileName($_); FileName = $_ } })
}

function Show-Processes {
    foreach ($name in $images) {
        foreach ($p in @(Get-Process -Name $name -ErrorAction SilentlyContinue)) {
            $start = try { $p.StartTime.ToUniversalTime().ToString('HH:mm:ssZ') } catch { '?' }
            Say ("process $name pid=" + $p.Id + ' session=' + $p.SessionId + ' start=' + $start)
            $mods = $null
            # Windows PowerShell turns a failed property read into a non-terminating error, so Stop makes it one.
            $saved = $ErrorActionPreference
            try {
                $ErrorActionPreference = 'Stop'
                $all = $p.Modules
                if ($null -eq $all) { throw 'no module list' }
                $mods = @($all | Where-Object { $_.ModuleName -match $moduleRx })
            } catch { $mods = $null } finally { $ErrorActionPreference = $saved }
            if ($null -eq $mods) {
                # The module list of another bitness or a process .NET cannot read: the in-process second reader.
                $all = Read-Modules $p.Id
                if ($null -eq $all) { continue }
                Say '  modules from the in-process reader'
                $mods = @($all | Where-Object { $_.ModuleName -match $moduleRx })
            }
            foreach ($m in $mods) {
                $h = try { (Get-FileHash -LiteralPath $m.FileName -Algorithm SHA256).Hash.Substring(0, 8) } catch { '?' }
                Say ('  module ' + $m.ModuleName + ' ' + $h + ' ' + $m.FileName)
            }
            $gpu = @($mods | Where-Object { $_.ModuleName -like 'amdgpu_wddm_d3d11*' }).Count
            $cpu = @($mods | Where-Object { $_.ModuleName -match '^bc250d3d(32)?\.dll$' }).Count
            Say ("  route witness: application GPU UMD loaded=" + [int]($gpu -gt 0) + ' CPU UMD loaded=' + [int]($cpu -gt 0))
        }
    }
}

function Show-RouteLogs {
    $dirs = @()
    foreach ($key in $appRouter, 'HKLM:\SOFTWARE\amdgpu-wddm\DesktopRouter') {
        $v = (Get-ItemProperty -LiteralPath $key -Name RouteLogDirectory -ErrorAction SilentlyContinue).RouteLogDirectory
        if ($v) { $dirs += $v }
    }
    if (-not $dirs.Count) { Say 'route logs: no RouteLogDirectory (run routelog-on first)'; return }
    foreach ($d in ($dirs | Select-Object -Unique)) {
        foreach ($f in @(Get-ChildItem -LiteralPath $d -Filter 'route-*.log' -ErrorAction SilentlyContinue |
                         Where-Object { $_.Name -match '^route-(logonui|lockapp|consent|credentialuibroker|winlogon)\.exe-' })) {
            foreach ($l in @(Get-Content -LiteralPath $f.FullName -Tail 4 -ErrorAction SilentlyContinue)) { Say ('route ' + $f.Name + ': ' + $l) }
        }
    }
}

function Snapshot([string]$label) {
    Say "---- $label"
    Show-StartState
    Show-AppRouter
    Show-Latch
    Show-Processes
    Show-RouteLogs
}

function Watch([string]$what) {
    $until = (Get-Date).AddSeconds($Seconds)
    $n = 0
    while ((Get-Date) -lt $until) {
        Start-Sleep -Seconds 5
        $n++
        Snapshot ("$what sample $n")
    }
}

function Get-ConsoleUser {
    # The owner of explorer.exe in the active console session.
    $session = $null
    try {
        Add-Type -Namespace LogonProbe -Name Wts -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint WTSGetActiveConsoleSessionId();' -ErrorAction Stop
        $session = [int][LogonProbe.Wts]::WTSGetActiveConsoleSessionId()
    } catch { $session = $null }
    $e = @(Get-CimInstance Win32_Process -Filter "Name='explorer.exe'" -ErrorAction SilentlyContinue | Where-Object { $null -eq $session -or $_.SessionId -eq $session })
    if (-not $e.Count) { return $null }
    $o = Invoke-CimMethod -InputObject $e[0] -MethodName GetOwner -ErrorAction SilentlyContinue
    if (-not $o -or -not $o.User) { return $null }
    return [pscustomobject]@{ User = ($o.Domain + '\' + $o.User); Session = $e[0].SessionId }
}

# One task: registered, started, given up to 20 s, removed. -System runs it as Local System; otherwise it runs in the
# console user's interactive session, which needs no password.
function Run-Task([string]$name, [string]$exe, [string]$arguments, [switch]$System) {
    $full = $taskPrefix + $name
    Unregister-ScheduledTask -TaskName $full -Confirm:$false -ErrorAction SilentlyContinue
    $act = New-ScheduledTaskAction -Execute $exe -Argument $arguments
    if ($System) { $pr = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest }
    else {
        $u = Get-ConsoleUser
        if (-not $u) { Say 'no console user (explorer.exe) found: nothing started'; return $false }
        Say ('console user ' + $u.User + ' session ' + $u.Session)
        $pr = New-ScheduledTaskPrincipal -UserId $u.User -LogonType Interactive -RunLevel Limited
    }
    $set = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 2)
    Register-ScheduledTask -TaskName $full -Action $act -Principal $pr -Settings $set -Force -ErrorAction SilentlyContinue | Out-Null
    if (-not (Get-ScheduledTask -TaskName $full -ErrorAction SilentlyContinue)) { Say "task $full not registered"; return $false }
    Start-ScheduledTask -TaskName $full -ErrorAction SilentlyContinue
    $deadline = (Get-Date).AddSeconds(20)
    do { Start-Sleep -Milliseconds 500; $info = Get-ScheduledTaskInfo -TaskName $full -ErrorAction SilentlyContinue }
    while ((Get-Date) -lt $deadline -and $info -and @(267009, 267011) -contains $info.LastTaskResult)
    Say ("task $full result=" + $(if ($info) { '0x{0:X}' -f $info.LastTaskResult } else { '?' }))
    Unregister-ScheduledTask -TaskName $full -Confirm:$false -ErrorAction SilentlyContinue
    return $true
}

function Stop-Image([string]$name) {
    $ps = @(Get-Process -Name $name -ErrorAction SilentlyContinue)
    if (-not $ps.Count) { Say "no $name process: nothing ended"; return }
    foreach ($p in $ps) {
        Say ("TerminateProcess $name pid=" + $p.Id)
        try { $p.Kill() } catch {
            Say ('  Kill failed: ' + $_.Exception.Message + '; taskkill')
            & taskkill.exe /f /pid $p.Id 2>&1 | ForEach-Object { Say ('  ' + $_) }
        }
    }
}

Say ("action=$Action seconds=$Seconds host=" + $env:COMPUTERNAME + ' ps=' + $PSVersionTable.PSVersion)
switch ($Action) {
    'snapshot' { Snapshot 'snapshot' }
    'lock' {
        Snapshot 'before lock'
        if (Run-Task 'lock' "$env:SystemRoot\System32\rundll32.exe" 'user32.dll,LockWorkStation') { Watch 'after lock' }
    }
    'unlock' {
        Snapshot 'before unlock'
        $u = Get-ConsoleUser
        $session = if ($u) { $u.Session } else { $null }
        if ($null -eq $session) {
            # A locked session still has its explorer.exe; this branch means no user session at all.
            Say 'no console session found: nothing reconnected'
        } elseif (Run-Task 'unlock' "$env:SystemRoot\System32\tscon.exe" ("$session /dest:console") -System) {
            Watch 'after unlock'
        }
    }
    'kill-logonui' { Snapshot 'before kill'; Stop-Image 'LogonUI'; Watch 'after kill' }
    'uac' {
        Snapshot 'before uac'
        # An elevation request in the user's session: consent.exe opens on the secure desktop and waits.
        if (Run-Task 'uac' "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" '-NoProfile -WindowStyle Hidden -Command "Start-Process -Verb RunAs -FilePath $env:SystemRoot\System32\cmd.exe -ArgumentList ''/c exit 0''"') { Watch 'after uac' }
    }
    'kill-consent' { Snapshot 'before kill'; Stop-Image 'consent'; Watch 'after kill' }
    'routelog-on' {
        $d = Join-Path $Out 'routelogs'
        New-Item -ItemType Directory -Force -Path $d | Out-Null
        # LogonUI.exe and consent.exe run as Local System, CredentialUIBroker.exe as the user and LockApp.exe in an
        # AppContainer (ALL APPLICATION PACKAGES, ALL RESTRICTED APPLICATION PACKAGES): all of them must write.
        & icacls.exe $d /grant '*S-1-5-32-545:(OI)(CI)M' '*S-1-15-2-1:(OI)(CI)M' '*S-1-15-2-2:(OI)(CI)M' | Out-Null
        # Never New-Item -Force on an existing key: the registry provider then creates the key again, empty, and the
        # installer's Mode, Allow and Deny values are gone (2026-11-05: the router then read Mode absent = cpu).
        if (-not (Test-Path -LiteralPath $appRouter)) { New-Item -Path $appRouter -ErrorAction SilentlyContinue | Out-Null }
        Set-ItemProperty -LiteralPath $appRouter -Name RouteLogDirectory -Value $d -Type String
        Say "AppRouter RouteLogDirectory=$d"
    }
    'routelog-off' { Remove-ItemProperty -LiteralPath $appRouter -Name RouteLogDirectory -ErrorAction SilentlyContinue; Show-AppRouter }
    'protected-off' { Set-ItemProperty -LiteralPath $appRouter -Name ProtectedGpu -Value 0 -Type DWord; Show-AppRouter }
    'protected-on' { Remove-ItemProperty -LiteralPath $appRouter -Name ProtectedGpu -ErrorAction SilentlyContinue; Show-AppRouter }
}
Say "report $report"
exit 0
