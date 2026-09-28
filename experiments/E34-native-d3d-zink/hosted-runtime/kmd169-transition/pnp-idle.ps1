# Contract: local windows-driver-docs staging110f60ea,
# install/checking-for-in-progress-installations.md; SDK26100 cfgmgr32.h:4637.
function Assert-KmdPnpIdleResult {
 param($Observation)
 if($null -eq $Observation -or $Observation.wait_status -isnot [uint32] -or
    $Observation.wait_status -ne 0){throw 'PnP installation activity not proven idle'}
}
function Get-KmdPnpIdle {
 if(-not ('KmdInstallEvents169' -as [type])){
  Add-Type 'using System;using System.Runtime.InteropServices;public static class KmdInstallEvents169{[DllImport("cfgmgr32.dll",ExactSpelling=true)]public static extern UInt32 CMP_WaitNoPendingInstallEvents(UInt32 timeout);}'
 }
 # No infinite wait; the whole phase is additionally inside a bounded job.
 $status=[KmdInstallEvents169]::CMP_WaitNoPendingInstallEvents(0)
 return @{wait_status=$status;qpc=[Diagnostics.Stopwatch]::GetTimestamp();
  installers=@(Get-Process drvinst -ErrorAction SilentlyContinue|ForEach-Object {@{pid=$_.Id;start=$_.StartTime.ToUniversalTime().ToString('o')}})}
}
