# LAB, interactive session (approute.py run exe powershell.exe -File ...): top-level windows of one process image
# (class, title, visible, iconic, rectangle, foreground) and the foreground window's owner; writes -Log.
param([string]$Image = 'ROTTR', [string]$Log = 'C:\BC250\tmp\windows-of.log')
Add-Type -Namespace Lab -Name Win -MemberDefinition @'
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L; public int T; public int R; public int B; }
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, System.IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool IsIconic(System.IntPtr h);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
'@
[void][Lab.Win]::SetProcessDPIAware()
$ids = @(Get-Process -Name $Image -ErrorAction SilentlyContinue | ForEach-Object { [uint32]$_.Id })
$fg = [Lab.Win]::GetForegroundWindow()
$out = New-Object System.Collections.Generic.List[string]
$out.Add(("{0:HH:mm:ss}Z pids {1}" -f [DateTime]::UtcNow, ($ids -join ',')))
$cb = [Lab.Win+EnumProc]{
    param($h, $l)
    $wp = [uint32]0; [void][Lab.Win]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($ids -contains $wp -or $h -eq $fg) {
        $c = New-Object System.Text.StringBuilder 256; $t = New-Object System.Text.StringBuilder 256
        [void][Lab.Win]::GetClassName($h, $c, 256); [void][Lab.Win]::GetWindowText($h, $t, 256)
        $r = New-Object Lab.Win+RECT; [void][Lab.Win]::GetWindowRect($h, [ref]$r)
        $out.Add(("pid {0} hwnd {1:X} class '{2}' title '{3}' visible {4} iconic {5} rect {6},{7}-{8},{9}{10}" -f $wp,
            $h.ToInt64(), $c, $t, [Lab.Win]::IsWindowVisible($h), [Lab.Win]::IsIconic($h), $r.L, $r.T, $r.R, $r.B,
            $(if ($h -eq $fg) { ' FOREGROUND' } else { '' })))
    }
    return $true
}
[void][Lab.Win]::EnumWindows($cb, [IntPtr]::Zero)
[IO.File]::WriteAllLines($Log, $out)
$out
