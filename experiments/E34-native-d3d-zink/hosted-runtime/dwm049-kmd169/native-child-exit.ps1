function Wait-NativeChildExit {
 param([Diagnostics.Process]$Process,[int]$TimeoutMilliseconds)
 # Retain a real process handle before waiting; PS5 Start-Process ExitCode can be null.
 $handle=$Process.Handle
 if(!$Process.WaitForExit($TimeoutMilliseconds)){throw 'Native client deadline'}
 if(!('DwmChildExitCode' -as [type])){Add-Type 'using System;using System.Runtime.InteropServices;public static class DwmChildExitCode{[DllImport("kernel32.dll",SetLastError=true)]public static extern bool GetExitCodeProcess(IntPtr process,out uint code);}' }
 [uint32]$code=0
 if(![DwmChildExitCode]::GetExitCodeProcess($handle,[ref]$code)){throw ('GetExitCodeProcess failed: '+[Runtime.InteropServices.Marshal]::GetLastWin32Error())}
 return $code
}
