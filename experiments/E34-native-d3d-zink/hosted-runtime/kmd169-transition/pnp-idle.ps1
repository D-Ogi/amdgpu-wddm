# Contract: local windows-driver-docs staging110f60ea,
# install/checking-for-in-progress-installations.md; SDK26100 cfgmgr32.h:4637.
function Assert-KmdPnpIdleResult {
 param($Observation)
 if($null -eq $Observation -or $Observation.wait_status -isnot [uint32] -or
    $Observation.wait_status -ne 0){throw 'PnP installation activity not proven idle'}
}
function Assert-KmdRestorableProblem {
 param($Problem)
 # SDK26100 shared/cfg.h: FAILED_START=10, FAILED_ADD=31, FAILED_POST_START=43.
 if($null -eq $Problem -or $Problem -notin @(0,10,22,31,43)){throw 'Device problem not admitted for automatic restoration'}
}
function Get-KmdPnpWaitMs {
 param([long]$Deadline)
 $remaining=($Deadline-[Diagnostics.Stopwatch]::GetTimestamp())*1000/[double][Diagnostics.Stopwatch]::Frequency
 # Leave five seconds for device checks, work and receipts. The outer job is final enforcement.
 $budget=[Math]::Min(10000,[Math]::Floor($remaining-5000))
 if($budget -le 0){throw 'No PnP wait budget'}
 return [uint32]$budget
}
function Get-KmdPnpIdle {
 param([Parameter(Mandatory)][long]$Deadline)
 if(-not ('KmdInstallEvents169' -as [type])){
  Add-Type 'using System;using System.Runtime.InteropServices;public static class KmdInstallEvents169{[DllImport("cfgmgr32.dll",ExactSpelling=true)]public static extern UInt32 CMP_WaitNoPendingInstallEvents(UInt32 timeout);}'
 }
 # No infinite wait; the whole phase is additionally inside a bounded job.
 $timeout=Get-KmdPnpWaitMs $Deadline
 $begin=[Diagnostics.Stopwatch]::GetTimestamp()
 $status=[KmdInstallEvents169]::CMP_WaitNoPendingInstallEvents($timeout)
 $waitMs=([Diagnostics.Stopwatch]::GetTimestamp()-$begin)*1000/[double][Diagnostics.Stopwatch]::Frequency
 return @{wait_status=$status;requested_ms=$timeout;elapsed_ms=$waitMs;qpc=[Diagnostics.Stopwatch]::GetTimestamp();
  installers=@(Get-Process drvinst -ErrorAction SilentlyContinue|ForEach-Object {@{pid=$_.Id;start=$_.StartTime.ToUniversalTime().ToString('o')}})}
}
