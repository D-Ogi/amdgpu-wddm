# LAB, interactive session (run through approute.py run exe powershell.exe -File ...): one left click at physical
# screen pixel X,Y (the process is made DPI aware first, so the coordinates are those of a full-resolution screenshot)
# and/or key taps by scan code. SendInput with an absolute move, a one-pixel wiggle (Chromium-based dialogs such as
# Steam's want a hover before the click) and the button pair; logs the cursor and the window under the point.
# Owner, 2026-10-01: the EULA of a test game may be accepted on the owner's behalf.
#   click.ps1 -X 960 -Y 600              click (also gives a fullscreen game the foreground)
#   click.ps1 -Keys 1C,01 -GapMs 800     Enter, then Esc (hex scan codes, as game-control.py uses)
# Start it hidden (267, The Ascent's EULA in Steam: a visible console window covered the button), from C:\BC250\tmp:
#   python scratch/m15/app-route/approute.py run exe --interactive --exe powershell.exe --seconds 20
#     --args "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File C:\BC250\tmp\click.ps1 -X <x> -Y <y>"
param([int]$X = -1, [int]$Y = -1, [string]$Keys = '', [int]$GapMs = 600, [string]$Log = 'C:\BC250\tmp\click.log')
function Note([string]$t) { Add-Content -Path $Log -Value ("{0:HH:mm:ss.fff}Z {1}" -f [DateTime]::UtcNow, $t) }
Note "start x=$X y=$Y keys=$Keys"
# INPUT is 40 bytes on x64: type at 0, the union (MOUSEINPUT 32 bytes, KEYBDINPUT 24) at 8.
Add-Type -Namespace Lab -Name Input -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
[StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public System.IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public System.IntPtr dwExtraInfo; }
[StructLayout(LayoutKind.Explicit, Size=40)] public struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; [FieldOffset(8)] public KEYBDINPUT ki; }
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
[DllImport("user32.dll")] public static extern System.IntPtr WindowFromPoint(POINT p);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
[DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
public static uint Mouse(int x, int y, uint flags) {
    int w = GetSystemMetrics(0), h = GetSystemMetrics(1);
    INPUT[] i = new INPUT[1];
    i[0].type = 0;
    i[0].mi.dx = (int)(((long)x * 65535) / (w - 1));
    i[0].mi.dy = (int)(((long)y * 65535) / (h - 1));
    i[0].mi.dwFlags = flags | 0x8000 | 0x0001; // ABSOLUTE | MOVE
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
}
public static uint Key(ushort scan, bool up) {
    INPUT[] i = new INPUT[1];
    i[0].type = 1;
    i[0].ki.wScan = scan;
    i[0].ki.dwFlags = 0x0008 | (up ? 0x0002u : 0u); // SCANCODE | KEYUP
    return SendInput(1, i, Marshal.SizeOf(typeof(INPUT)));
}
public static string Describe(System.IntPtr h) {
    var c = new System.Text.StringBuilder(256); var t = new System.Text.StringBuilder(256);
    GetClassName(h, c, 256); GetWindowText(h, t, 256);
    return "class '" + c + "' title '" + t + "'";
}
'@
[void][Lab.Input]::SetProcessDPIAware()
Note ("screen {0}x{1}, input size {2}, foreground {3}" -f [Lab.Input]::GetSystemMetrics(0), [Lab.Input]::GetSystemMetrics(1),
    [Runtime.InteropServices.Marshal]::SizeOf([type][Lab.Input+INPUT]), [Lab.Input]::Describe([Lab.Input]::GetForegroundWindow()))
if ($X -ge 0 -and $Y -ge 0) {
    $pt = New-Object Lab.Input+POINT; $pt.X = $X; $pt.Y = $Y
    Note ("window under point: " + [Lab.Input]::Describe([Lab.Input]::WindowFromPoint($pt)))
    $sent = @()
    $sent += [Lab.Input]::Mouse($X - 3, $Y - 2, 0)
    Start-Sleep -Milliseconds 120
    $sent += [Lab.Input]::Mouse($X, $Y, 0)
    Start-Sleep -Milliseconds 200
    $sent += [Lab.Input]::Mouse($X, $Y, 0x0002)
    Start-Sleep -Milliseconds 80
    $sent += [Lab.Input]::Mouse($X, $Y, 0x0004)
    $c = New-Object Lab.Input+POINT; [void][Lab.Input]::GetCursorPos([ref]$c)
    Note ("click sent {0}, cursor now {1},{2}" -f ($sent -join '/'), $c.X, $c.Y)
    Start-Sleep -Milliseconds 400
}
foreach ($k in ($Keys -split ',' | Where-Object { $_ })) {
    $scan = [Convert]::ToUInt16($k, 16)
    $a = [Lab.Input]::Key($scan, $false); Start-Sleep -Milliseconds 70; $b = [Lab.Input]::Key($scan, $true)
    Note ("key {0} sent {1}/{2}" -f $k, $a, $b)
    Start-Sleep -Milliseconds $GapMs
}
Note ("end, foreground " + [Lab.Input]::Describe([Lab.Input]::GetForegroundWindow()))
