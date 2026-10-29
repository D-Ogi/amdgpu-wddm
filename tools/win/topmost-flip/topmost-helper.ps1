# The console-session half of topmost-flip.ps1 (BD-187). Do not run it by hand: topmost-flip.ps1 copies it into its
# work folder and starts it through a scheduled task of the console user with the highest run level, because Task
# Manager runs elevated and a window of a lower integrity level cannot change it (UIPI).
#
# It starts Task Manager, then waits for command files from the session-0 script: cmd-<n>.txt with one word, on, off
# or quit. For on and off it sets Task Manager's own "Always on top" option through UI Automation (the Settings page,
# the check box -ToggleName), and checks WS_EX_TOPMOST of the window afterwards. When UI Automation does not find the
# check box, or the window style does not follow it, it sets the style itself with SetWindowPos and says so: method
# "window" is the mechanism, not the reported option. Each command gets result-<n>.json. quit turns the option off,
# closes the window with WM_CLOSE and ends. The helper also ends at -Seconds, the same way.
#
# A hung window answers neither UI Automation nor SetWindowPos in time: in run 1 of 2026-10-29 each FromHandle took
# about 20 s and the window stayed blank. So every command first sends WM_NULL with SendMessageTimeout (2 s, abort if
# hung). A window that does not answer gets no UI Automation and no SetWindowPos, and its result says hung. UI
# Automation runs in its own runspace and is given up after -UiaSeconds. Each answer comes in about 10 s or less.
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$SettingsName = 'Settings',
    [string]$ToggleName = 'Always on top',
    [int]$Seconds = 160,
    [int]$UiaSeconds = 6
)
$ErrorActionPreference = 'Continue'
$deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
$logPath = Join-Path $Dir 'helper.log'

function Write-HelperLog([string]$Text) {
    Add-Content -LiteralPath $logPath -Value ('{0} {1}' -f [DateTime]::UtcNow.ToString('o'), $Text) -Encoding ascii
}

function Write-Json([string]$Name, $Value) {
    $path = Join-Path $Dir $Name
    $temporary = $path + '.tmp'
    Set-Content -LiteralPath $temporary -Value ($Value | ConvertTo-Json -Compress) -Encoding ascii
    Move-Item -LiteralPath $temporary -Destination $path -Force
}

Add-Type -Namespace Bc250 -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr hwnd, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hwnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetWindowTextLength(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassName(System.IntPtr hwnd, System.Text.StringBuilder name, int count);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetWindowLong(System.IntPtr hwnd, int index);
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
public static extern bool SetWindowPos(System.IntPtr hwnd, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l);
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l, uint flags, uint ms, out System.IntPtr result);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsHungAppWindow(System.IntPtr hwnd);
// Read only, and not documented: the z-order band of a window (1 is the desktop band). It says whether Task Manager's
// option moves the window to another band instead of setting WS_EX_TOPMOST. -1 when the export is absent.
[System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "GetWindowBand")]
static extern bool GetWindowBandRaw(System.IntPtr hwnd, out uint band);
public static long Band(System.IntPtr hwnd) {
    try { uint band; return GetWindowBandRaw(hwnd, out band) ? (long)band : -1; } catch (System.EntryPointNotFoundException) { return -1; }
}
public static string ClassOf(System.IntPtr hwnd) {
    var name = new System.Text.StringBuilder(256); GetClassName(hwnd, name, name.Capacity); return name.ToString();
}
// The top-level window of the process that has a title: TaskManagerWindow when there is one, else the first.
public static System.IntPtr MainWindow(uint pid) {
    System.IntPtr found = System.IntPtr.Zero, any = System.IntPtr.Zero;
    EnumWindows(delegate(System.IntPtr h, System.IntPtr l) {
        uint owner; GetWindowThreadProcessId(h, out owner);
        if (owner == pid && IsWindowVisible(h) && GetWindowTextLength(h) > 0) {
            if (ClassOf(h) == "TaskManagerWindow") { found = h; return false; }
            if (any == System.IntPtr.Zero) any = h;
        }
        return true;
    }, System.IntPtr.Zero);
    return found != System.IntPtr.Zero ? found : any;
}
'@

$WS_EX_TOPMOST = 0x8
function Get-Topmost([IntPtr]$Window) { return (([Bc250.Win]::GetWindowLong($Window, -20)) -band $WS_EX_TOPMOST) -ne 0 }

# WM_NULL through SendMessageTimeout with SMTO_ABORTIFHUNG: TRUE when the window's thread answers within $Ms.
function Test-Responsive([IntPtr]$Window, [int]$Ms = 2000) {
    $reply = [IntPtr]::Zero
    $answer = [Bc250.Win]::SendMessageTimeout($Window, 0, [IntPtr]::Zero, [IntPtr]::Zero, 0x2, [uint32]$Ms, [ref]$reply)
    $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    $ok = $answer -ne [IntPtr]::Zero
    if (-not $ok) { Write-HelperLog "wm_null: no answer in $Ms ms, error $errorCode, IsHungAppWindow $([Bc250.Win]::IsHungAppWindow($Window))" }
    return $ok
}

# Task Manager's own option, in a runspace of its own so that a provider that does not answer costs at most
# $UiaSeconds. The check box is on the Settings page, which is opened only when the box is not on the page shown.
# Returns the toggle state after the change, 'not-found:<what>' or 'timeout'.
$uiaScript = {
    param([long]$Handle, [bool]$On, [string]$SettingsName, [string]$ToggleName)
    Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
    $root = [Windows.Automation.AutomationElement]::FromHandle([IntPtr]$Handle)
    $scope = [Windows.Automation.TreeScope]::Descendants
    $byName = { param($name) New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $name) }
    $box = $root.FindFirst($scope, (& $byName $ToggleName))
    if (-not $box) {
        $nav = $root.FindFirst($scope, (& $byName $SettingsName))
        if (-not $nav) { return "not-found:$SettingsName" }
        $pattern = $null
        if ($nav.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) { $pattern.Select() }
        elseif ($nav.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) { $pattern.Invoke() }
        else { return "not-found:pattern of $SettingsName" }
        $until = [DateTime]::UtcNow.AddSeconds(3)
        do { Start-Sleep -Milliseconds 250; $box = $root.FindFirst($scope, (& $byName $ToggleName)) } while (-not $box -and [DateTime]::UtcNow -lt $until)
        if (-not $box) { return "not-found:$ToggleName" }
    }
    $toggle = $null
    if (-not $box.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$toggle)) { return "not-found:toggle pattern" }
    $want = if ($On) { [Windows.Automation.ToggleState]::On } else { [Windows.Automation.ToggleState]::Off }
    if ($toggle.Current.ToggleState -ne $want) { $toggle.Toggle() }
    Start-Sleep -Milliseconds 400
    return [string]$toggle.Current.ToggleState
}

function Set-Option([IntPtr]$Window, [bool]$On) {
    $shell = [PowerShell]::Create()
    [void]$shell.AddScript($uiaScript).AddArgument([int64]$Window).AddArgument($On).AddArgument($SettingsName).AddArgument($ToggleName)
    $pending = $shell.BeginInvoke()
    if (-not $pending.AsyncWaitHandle.WaitOne($UiaSeconds * 1000)) {
        # Abandoned, not stopped: a Stop would wait for the same provider. The runspace ends with the helper.
        [void]$shell.BeginStop($null, $null)
        return 'timeout'
    }
    try {
        $out = @($shell.EndInvoke($pending))
        foreach ($e in $shell.Streams.Error) { Write-HelperLog "uia: $e" }
        $shell.Dispose()
        return $(if ($out.Count) { [string]$out[-1] } else { 'no-result' })
    } catch { return "error:$($_.Exception.InnerException.Message)" }
}

function Invoke-Topmost([IntPtr]$Window, [bool]$On) {
    $before = [pscustomobject]@{ topmost = (Get-Topmost $Window); band = [Bc250.Win]::Band($Window) }
    if (-not (Test-Responsive $Window)) {
        return [pscustomobject]@{ method = 'none'; hung = $true; option = $null; setwindowpos = $null; win32_error = $null
            topmost = (Get-Topmost $Window); band = [Bc250.Win]::Band($Window); before = $before }
    }
    $state = Set-Option $Window $On
    $method = 'option'
    $set = $null
    $errorCode = $null
    if ($state -notin @('On', 'Off') -or (Get-Topmost $Window) -ne $On) {
        # The option did not take, or it does not use WS_EX_TOPMOST. Set the style itself, synchronously, and only
        # after another WM_NULL check, so that a window that hung meanwhile does not block this call.
        # HWND_TOPMOST -1, HWND_NOTOPMOST -2; SWP_NOSIZE 1, SWP_NOMOVE 2, SWP_NOACTIVATE 0x10.
        $method = 'window'
        if (Test-Responsive $Window) {
            $after = if ($On) { [IntPtr](-1) } else { [IntPtr](-2) }
            $set = [Bc250.Win]::SetWindowPos($Window, $after, 0, 0, 0, 0, 0x13)
            $errorCode = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            Write-HelperLog "setwindowpos $(if ($On) { 'topmost' } else { 'notopmost' }) -> $set, error $errorCode, topmost $(Get-Topmost $Window)"
        } else { $method = 'none' }
    }
    return [pscustomobject]@{ method = $method; hung = $false; option = $state; setwindowpos = $set; win32_error = $errorCode
        topmost = (Get-Topmost $Window); band = [Bc250.Win]::Band($Window); before = $before }
}

$elevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-HelperLog "start dir $Dir settings '$SettingsName' toggle '$ToggleName' seconds $Seconds uia $UiaSeconds s elevated $elevated"
if (Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue) {
    Write-Json 'ready.json' ([pscustomobject]@{ ok = $false; reason = 'Task Manager already runs' }); exit 2
}
$process = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\Taskmgr.exe') -PassThru
$window = [IntPtr]::Zero
$until = [DateTime]::UtcNow.AddSeconds(20)
do {
    Start-Sleep -Milliseconds 250
    # Taskmgr.exe can hand over to a second instance; take the window of whichever Taskmgr runs.
    foreach ($p in @(Get-Process -Name 'Taskmgr' -ErrorAction SilentlyContinue)) {
        $window = [Bc250.Win]::MainWindow([uint32]$p.Id)
        if ($window -ne [IntPtr]::Zero) { $process = $p; break }
    }
} while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $until)
if ($window -eq [IntPtr]::Zero) {
    Write-Json 'ready.json' ([pscustomobject]@{ ok = $false; reason = 'no Task Manager window in 20 s' }); exit 1
}
# A window can be slow at its first paint; it gets 10 s to answer WM_NULL before the trial calls it hung.
$responsive = $false
$until = [DateTime]::UtcNow.AddSeconds(10)
do { $responsive = Test-Responsive $window 1000; if (-not $responsive) { Start-Sleep -Milliseconds 500 } } while (-not $responsive -and [DateTime]::UtcNow -lt $until)
$windowClass = [Bc250.Win]::ClassOf($window)
Write-Json 'ready.json' ([pscustomobject]@{ ok = $true; pid = $process.Id; window = ('0x{0:X}' -f [int64]$window); class = $windowClass
    responsive = $responsive; topmost = (Get-Topmost $window); band = [Bc250.Win]::Band($window); elevated = $elevated })
Write-HelperLog "taskmgr pid $($process.Id) window 0x$('{0:X}' -f [int64]$window) class $windowClass responsive $responsive topmost $(Get-Topmost $window) band $([Bc250.Win]::Band($window))"

$n = 1
$quit = $false
while (-not $quit) {
    $command = Join-Path $Dir ('cmd-{0}.txt' -f $n)
    if ([DateTime]::UtcNow -ge $deadline) { $word = 'quit'; Write-HelperLog 'deadline: quit' }
    elseif (Test-Path -LiteralPath $command) { $word = (Get-Content -LiteralPath $command -Raw).Trim() }
    else { Start-Sleep -Milliseconds 200; continue }
    $began = [DateTime]::UtcNow
    $result = $null
    switch ($word) {
        'on' { $result = Invoke-Topmost $window $true }
        'off' { $result = Invoke-Topmost $window $false }
        'quit' {
            # The option is persistent in Task Manager: it goes back off before the window closes.
            $result = Invoke-Topmost $window $false
            [void][Bc250.Win]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
            $ended = $process.WaitForExit(6000)
            $result | Add-Member -NotePropertyName ended -NotePropertyValue $ended
            $quit = $true
        }
        default { $result = [pscustomobject]@{ method = 'none'; error = "unknown command '$word'" } }
    }
    $result | Add-Member -NotePropertyName command -NotePropertyValue $word
    $result | Add-Member -NotePropertyName ms -NotePropertyValue ([int]([DateTime]::UtcNow - $began).TotalMilliseconds)
    $result | Add-Member -NotePropertyName utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
    Write-Json ('result-{0}.json' -f $n) $result
    Write-HelperLog "cmd $n $word -> $($result | ConvertTo-Json -Compress)"
    $n++
}
exit 0
