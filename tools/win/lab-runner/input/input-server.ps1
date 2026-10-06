# LAB, interactive session 1 (started by start-input.ps1 as a one-shot scheduled task of the lab user): a bounded
# input server for game sessions that are not native-caps trials (Steam games through the M14.1/M15 routes). It polls
# C:\BC250\tmp\control\<Session>\cmd-NNN.txt every 100 ms, runs the actions with SendInput and writes done-NNN.txt.
# Same action language as game-runtime.ps1's interactive channel (game-recon\game-control.py), minus shot/ocr (the
# host takes screenshots with mon.py):
#   tap:SCAN  tapx:SCAN (extended: arrows E0 48/50/4B/4D, Home, End...)  hold:SCAN:MS  holdx:SCAN:MS
#   click:left|right[:MS]  clickat:FX:FY[:MS] (fractions of the screen, left button)  point:FX:FY
#   look:DX:DY:N:MS (relative mouse steps)  wheel:N (+up/-down notches)  wait:MS  quit
# Buttons and keys are held 150-250 ms by default: a game at a few frames per second misses shorter presses.
# Owner, 2026-10-01: game sessions up to 1200 s; this server ends at -Seconds whatever happens.
param([Parameter(Mandatory)][string]$Session, [int]$Seconds = 900)
$ErrorActionPreference = 'Stop'
if ($Session -notmatch '^game-[a-z0-9-]{3,40}$') { throw 'bad session name' }
$Seconds = [Math]::Min([Math]::Max($Seconds, 10), 1200)
$dir = "C:\BC250\tmp\control\$Session"
$null = New-Item -ItemType Directory -Force -Path $dir
$log = Join-Path $dir 'server.log'
function Note([string]$t) { Add-Content -Path $log -Value ("{0:HH:mm:ss.fff}Z {1}" -f [DateTime]::UtcNow, $t) }
Add-Type -Namespace Lab -Name In -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public int mouseData; public uint dwFlags; public uint time; public System.IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public System.IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; [FieldOffset(8)] public KEYBDINPUT ki; }
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
static uint One(INPUT i) { return SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT))); }
public static uint Key(ushort scan, bool ext, bool up) {
    INPUT i = new INPUT(); i.type = 1; i.ki.wScan = scan;
    i.ki.dwFlags = 0x0008u | (ext ? 0x0001u : 0u) | (up ? 0x0002u : 0u); // SCANCODE | EXTENDED | KEYUP
    return One(i);
}
public static uint MoveAbs(double fx, double fy) {
    INPUT i = new INPUT(); i.type = 0;
    i.mi.dx = (int)(fx * 65535.0); i.mi.dy = (int)(fy * 65535.0);
    i.mi.dwFlags = 0x8000u | 0x0001u; // ABSOLUTE | MOVE
    return One(i);
}
public static uint MoveRel(int dx, int dy) { INPUT i = new INPUT(); i.type = 0; i.mi.dx = dx; i.mi.dy = dy; i.mi.dwFlags = 0x0001u; return One(i); }
public static uint Button(bool right, bool up) {
    INPUT i = new INPUT(); i.type = 0;
    i.mi.dwFlags = right ? (up ? 0x0010u : 0x0008u) : (up ? 0x0004u : 0x0002u);
    return One(i);
}
public static uint Wheel(int notches) { INPUT i = new INPUT(); i.type = 0; i.mi.mouseData = notches * 120; i.mi.dwFlags = 0x0800u; return One(i); }
public static string Foreground() { var t = new System.Text.StringBuilder(256); GetWindowText(GetForegroundWindow(), t, 256); return t.ToString(); }
'@
[void][Lab.In]::SetProcessDPIAware()
function Hex([string]$s) { [Convert]::ToUInt16($s, 16) }
function Run-Action([string]$a) {
    $p = $a.Trim() -split ':'
    switch ($p[0]) {
        'tap'     { $x = [Lab.In]::Key((Hex $p[1]), $false, $false); Start-Sleep -Milliseconds 150; $y = [Lab.In]::Key((Hex $p[1]), $false, $true); return "tap $($p[1]) $x/$y" }
        'tapx'    { $x = [Lab.In]::Key((Hex $p[1]), $true, $false); Start-Sleep -Milliseconds 150; $y = [Lab.In]::Key((Hex $p[1]), $true, $true); return "tapx $($p[1]) $x/$y" }
        'hold'    { $x = [Lab.In]::Key((Hex $p[1]), $false, $false); Start-Sleep -Milliseconds ([int]$p[2]); $y = [Lab.In]::Key((Hex $p[1]), $false, $true); return "hold $($p[1]) $($p[2]) $x/$y" }
        'holdx'   { $x = [Lab.In]::Key((Hex $p[1]), $true, $false); Start-Sleep -Milliseconds ([int]$p[2]); $y = [Lab.In]::Key((Hex $p[1]), $true, $true); return "holdx $($p[1]) $($p[2]) $x/$y" }
        'point'   { $x = [Lab.In]::MoveAbs([double]$p[1], [double]$p[2]); return "point $($p[1]),$($p[2]) $x" }
        'click'   { $ms = if ($p.Count -gt 2) { [int]$p[2] } else { 250 }; $r = $p[1] -eq 'right'
                    $x = [Lab.In]::Button($r, $false); Start-Sleep -Milliseconds $ms; $y = [Lab.In]::Button($r, $true); return "click $($p[1]) $ms $x/$y" }
        'clickat' { $ms = if ($p.Count -gt 3) { [int]$p[3] } else { 250 }
                    $m1 = [Lab.In]::MoveAbs([double]$p[1] - 0.002, [double]$p[2] - 0.002); Start-Sleep -Milliseconds 120
                    $m2 = [Lab.In]::MoveAbs([double]$p[1], [double]$p[2]); Start-Sleep -Milliseconds 250
                    $x = [Lab.In]::Button($false, $false); Start-Sleep -Milliseconds $ms; $y = [Lab.In]::Button($false, $true)
                    return "clickat $($p[1]),$($p[2]) $ms $m1/$m2/$x/$y" }
        'look'    { $n = [int]$p[3]; $ok = 0; for ($i = 0; $i -lt $n; $i++) { $ok += [Lab.In]::MoveRel([int]$p[1], [int]$p[2]); Start-Sleep -Milliseconds ([int]$p[4]) }; return "look $($p[1]),$($p[2]) x$n $ok" }
        'wheel'   { $x = [Lab.In]::Wheel([int]$p[1]); return "wheel $($p[1]) $x" }
        'wait'    { Start-Sleep -Milliseconds ([int]$p[1]); return "wait $($p[1])" }
        'quit'    { $script:quit = $true; return 'quit' }
        default   { return "refused '$a'" }
    }
}
$script:quit = $false
$sw = [Diagnostics.Stopwatch]::StartNew()
Note "start session $Session bound $Seconds s, screen $([Lab.In]::GetSystemMetrics(0))x$([Lab.In]::GetSystemMetrics(1)), foreground '$([Lab.In]::Foreground())'"
$n = 1
while (-not $script:quit -and $sw.Elapsed.TotalSeconds -lt $Seconds) {
    $cmd = Join-Path $dir ('cmd-{0:d3}.txt' -f $n)
    if (-not (Test-Path -LiteralPath $cmd)) { Start-Sleep -Milliseconds 100; continue }
    Start-Sleep -Milliseconds 30
    $text = [IO.File]::ReadAllText($cmd)
    $out = New-Object System.Collections.Generic.List[string]
    $t0 = $sw.Elapsed.TotalMilliseconds
    foreach ($a in ($text -split ';' | Where-Object { $_.Trim() })) {
        try { $out.Add((Run-Action $a)) } catch { $out.Add("error '$a': $($_.Exception.Message)") }
        if ($script:quit) { break }
    }
    $out.Add(("{0} actions in {1:N0} ms, foreground '{2}', server t={3:N0} s" -f $out.Count, ($sw.Elapsed.TotalMilliseconds - $t0), [Lab.In]::Foreground(), $sw.Elapsed.TotalSeconds))
    [IO.File]::WriteAllLines((Join-Path $dir ('done-{0:d3}.txt' -f $n)), $out)
    Note ("cmd-{0:d3}: {1}" -f $n, ($out -join ' | '))
    $n++
}
Note ("end after {0:N0} s ({1})" -f $sw.Elapsed.TotalSeconds, $(if ($script:quit) { 'quit' } else { 'bound' }))
