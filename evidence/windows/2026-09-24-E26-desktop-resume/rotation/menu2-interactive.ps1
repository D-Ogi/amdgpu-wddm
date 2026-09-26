$ErrorActionPreference='Stop'
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public class M13Input{[DllImport("user32.dll")]public static extern void keybd_event(byte k,byte s,uint f,UIntPtr e);}'
'open='+(Get-Date).ToString('s') | Set-Content C:\BC250\m13\menu-control\keys2.txt
[M13Input]::keybd_event(0x5b,0,0,[UIntPtr]::Zero)
Start-Sleep -Milliseconds 100
[M13Input]::keybd_event(0x5b,0,2,[UIntPtr]::Zero)
Start-Sleep -Seconds 14
[M13Input]::keybd_event(0x1b,0,0,[UIntPtr]::Zero)
[M13Input]::keybd_event(0x1b,0,2,[UIntPtr]::Zero)
'closed='+(Get-Date).ToString('s') | Add-Content C:\BC250\m13\menu-control\keys2.txt
