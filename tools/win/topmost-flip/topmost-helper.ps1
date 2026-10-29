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
param(
    [Parameter(Mandatory = $true)][string]$Dir,
    [string]$SettingsName = 'Settings',
    [string]$ToggleName = 'Always on top',
    [int]$Seconds = 160
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

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -Namespace Bc250 -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr hwnd, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr hwnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetWindowTextLength(System.IntPtr hwnd);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern int GetWindowLong(System.IntPtr hwnd, int index);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SetWindowPos(System.IntPtr hwnd, System.IntPtr after, int x, int y, int cx, int cy, uint flags);
[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr hwnd, uint msg, System.IntPtr w, System.IntPtr l);
public static System.IntPtr MainWindow(uint pid) {
    System.IntPtr found = System.IntPtr.Zero;
    EnumWindows(delegate(System.IntPtr h, System.IntPtr l) {
        uint owner; GetWindowThreadProcessId(h, out owner);
        if (owner == pid && IsWindowVisible(h) && GetWindowTextLength(h) > 0) { found = h; return false; }
        return true;
    }, System.IntPtr.Zero);
    return found;
}
'@

$WS_EX_TOPMOST = 0x8
function Get-Topmost([IntPtr]$Window) { return (([Bc250.Win]::GetWindowLong($Window, -20)) -band $WS_EX_TOPMOST) -ne 0 }

# Task Manager's own option. The check box is on the Settings page; the page is opened only when the box is not found
# on the page that is shown. Returns the toggle state after the change, or $null when the box was not found.
function Set-Option([IntPtr]$Window, [bool]$On) {
    $root = [Windows.Automation.AutomationElement]::FromHandle($Window)
    $byName = { param($name) New-Object Windows.Automation.PropertyCondition([Windows.Automation.AutomationElement]::NameProperty, $name) }
    $scope = [Windows.Automation.TreeScope]::Descendants
    $box = $root.FindFirst($scope, (& $byName $ToggleName))
    if (-not $box) {
        $nav = $root.FindFirst($scope, (& $byName $SettingsName))
        if (-not $nav) { Write-HelperLog "uia: no element '$SettingsName'"; return $null }
        $pattern = $null
        if ($nav.TryGetCurrentPattern([Windows.Automation.SelectionItemPattern]::Pattern, [ref]$pattern)) { $pattern.Select() }
        elseif ($nav.TryGetCurrentPattern([Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)) { $pattern.Invoke() }
        else { Write-HelperLog "uia: '$SettingsName' has no select or invoke pattern"; return $null }
        $until = [DateTime]::UtcNow.AddSeconds(4)
        do { Start-Sleep -Milliseconds 250; $box = $root.FindFirst($scope, (& $byName $ToggleName)) } while (-not $box -and [DateTime]::UtcNow -lt $until)
        if (-not $box) { Write-HelperLog "uia: no element '$ToggleName' on the settings page"; return $null }
    }
    $toggle = $null
    if (-not $box.TryGetCurrentPattern([Windows.Automation.TogglePattern]::Pattern, [ref]$toggle)) {
        Write-HelperLog "uia: '$ToggleName' has no toggle pattern"; return $null
    }
    $want = if ($On) { [Windows.Automation.ToggleState]::On } else { [Windows.Automation.ToggleState]::Off }
    if ($toggle.Current.ToggleState -ne $want) { $toggle.Toggle() }
    Start-Sleep -Milliseconds 400
    return [string]$toggle.Current.ToggleState
}

function Invoke-Topmost([IntPtr]$Window, [bool]$On) {
    $state = $null
    try { $state = Set-Option $Window $On } catch { Write-HelperLog "uia: $($_.Exception.Message)" }
    $method = 'option'
    if ($null -eq $state -or (Get-Topmost $Window) -ne $On) {
        # HWND_TOPMOST -1, HWND_NOTOPMOST -2; SWP_NOSIZE 1, SWP_NOMOVE 2, SWP_NOACTIVATE 0x10.
        $after = if ($On) { [IntPtr](-1) } else { [IntPtr](-2) }
        [void][Bc250.Win]::SetWindowPos($Window, $after, 0, 0, 0, 0, 0x13)
        $method = 'window'
    }
    return [pscustomobject]@{ method = $method; option = $state; topmost = (Get-Topmost $Window) }
}

Write-HelperLog "start dir $Dir settings '$SettingsName' toggle '$ToggleName' seconds $Seconds"
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
Write-Json 'ready.json' ([pscustomobject]@{ ok = $true; pid = $process.Id; window = ('0x{0:X}' -f [int64]$window); topmost = (Get-Topmost $window) })
Write-HelperLog "taskmgr pid $($process.Id) window 0x$('{0:X}' -f [int64]$window) topmost $(Get-Topmost $window)"

$n = 1
$quit = $false
while (-not $quit) {
    $command = Join-Path $Dir ('cmd-{0}.txt' -f $n)
    if ([DateTime]::UtcNow -ge $deadline) { $word = 'quit'; Write-HelperLog 'deadline: quit' }
    elseif (Test-Path -LiteralPath $command) { $word = (Get-Content -LiteralPath $command -Raw).Trim() }
    else { Start-Sleep -Milliseconds 200; continue }
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
    $result | Add-Member -NotePropertyName utc -NotePropertyValue ([DateTime]::UtcNow.ToString('o'))
    Write-Json ('result-{0}.json' -f $n) $result
    Write-HelperLog "cmd $n $word -> $($result | ConvertTo-Json -Compress)"
    $n++
}
exit 0
