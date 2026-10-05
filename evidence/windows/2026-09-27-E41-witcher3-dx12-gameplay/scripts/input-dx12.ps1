# Send one input to the running witcher3 window (runs inside the interactive session as task BC250-Witcher-Input).
# Derived from the recorded DX11 input.ps1. Actions: Click (X,Y in 1920x1200 desktop pixels), Enter, Escape,
# Key (virtual key code -Vk, e.g. 69 = E, 32 = space), Hold (key held for -Ms milliseconds, e.g. W to walk).
param([ValidateSet('Click', 'Enter', 'Escape', 'Key', 'Hold')][string]$Action, [int]$X = 0, [int]$Y = 0, [int]$Vk = 0, [int]$Ms = 1000)
$ErrorActionPreference = 'Stop'
$out = 'C:\BC250\m12\witcher3-dx12'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class GameInput {
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
 [DllImport("user32.dll")] public static extern void keybd_event(byte key,byte scan,uint flags,UIntPtr extra);
}
"@
try {
  $p = Get-Process witcher3 -ErrorAction Stop
  if (@($p).Count -ne 1 -or $p.SessionId -ne [Diagnostics.Process]::GetCurrentProcess().SessionId) { throw 'Game session mismatch' }
  [GameInput]::keybd_event(18, 0, 0, [UIntPtr]::Zero); [GameInput]::keybd_event(18, 0, 2, [UIntPtr]::Zero)
  [GameInput]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
  Start-Sleep -Milliseconds 250
  [uint32]$fg = 0; [GameInput]::GetWindowThreadProcessId([GameInput]::GetForegroundWindow(), [ref]$fg) | Out-Null
  if ($fg -ne $p.Id) { throw 'Game is not foreground; no input sent' }
  switch ($Action) {
    'Click' {
      [GameInput]::SetCursorPos($X, $Y) | Out-Null
      [GameInput]::mouse_event(32769, [uint32]($X * 65535 / 1920), [uint32]($Y * 65535 / 1200), 0, [UIntPtr]::Zero)
      Start-Sleep -Milliseconds 500
      [GameInput]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [GameInput]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
    }
    'Enter' { [GameInput]::keybd_event(13, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [GameInput]::keybd_event(13, 0, 2, [UIntPtr]::Zero) }
    'Escape' { [GameInput]::keybd_event(27, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [GameInput]::keybd_event(27, 0, 2, [UIntPtr]::Zero) }
    'Key' { [GameInput]::keybd_event([byte]$Vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 80; [GameInput]::keybd_event([byte]$Vk, 0, 2, [UIntPtr]::Zero) }
    'Hold' { [GameInput]::keybd_event([byte]$Vk, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds $Ms; [GameInput]::keybd_event([byte]$Vk, 0, 2, [UIntPtr]::Zero) }
  }
  "$([DateTime]::UtcNow.ToString('o')) $Action X=$X Y=$Y Vk=$Vk Ms=$Ms ok" | Add-Content "$out\input-002.log"
} catch {
  "$([DateTime]::UtcNow.ToString('o')) $Action X=$X Y=$Y Vk=$Vk error: $($_.ToString())" | Add-Content "$out\input-002.log"
  throw
}
