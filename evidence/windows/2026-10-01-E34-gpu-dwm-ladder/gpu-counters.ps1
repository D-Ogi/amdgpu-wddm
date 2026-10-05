param([string]$Trial='x',[int]$Seconds=150)
# Samples the counters behind Task Manager's GPU page for the BC-250 adapter once a second, in one process.
# Output: C:\BC250\tmp\gpuctr-<Trial>.txt (UTC, 3D %, Copy %, dedicated MB, shared MB, top 3D process).
$out="C:\BC250\tmp\gpuctr-$Trial.txt"
$luid=$null
$cli=@('C:\BC250\dpm\bc250kmd_cli.exe','C:\BC250\bc250kmd_cli.exe') | Where-Object { Test-Path $_ } | Select-Object -First 1
if($cli){ $l=(& $cli list 2>$null | Select-String 'luid 00000000-([0-9A-F]+)' | Select-Object -Last 1); if($l){ $luid='0x'+$l.Matches[0].Groups[1].Value } }
if(-not $luid){ "no BC-250 luid" | Set-Content $out; exit 1 }
"luid $luid seconds $Seconds start $((Get-Date).ToUniversalTime().ToString('o'))" | Set-Content $out
$end=(Get-Date).AddSeconds($Seconds)
while((Get-Date) -lt $end){
  try{
    $all=(Get-Counter -Counter '\GPU Engine(*)\Utilization Percentage','\GPU Adapter Memory(*)\Dedicated Usage','\GPU Adapter Memory(*)\Shared Usage','\Process(dwm)\% Processor Time','\Processor(_Total)\% Processor Time' -ErrorAction Stop).CounterSamples
    $s=$all | Where-Object { $_.Path -like '*gpu*' }
    $dwm=($all | Where-Object { $_.Path -like '*\process(dwm)\*' } | Measure-Object CookedValue -Sum).Sum
    $cpu=($all | Where-Object { $_.Path -like '*\processor(_total)\*' } | Measure-Object CookedValue -Sum).Sum
    $mine=$s | Where-Object { $_.InstanceName -like "*luid_0x00000000_$($luid.ToLower())*" }
    $e3=$mine | Where-Object { $_.Path -like '*utilization*' -and $_.InstanceName -like '*engtype_3d' }
    $ec=$mine | Where-Object { $_.Path -like '*utilization*' -and $_.InstanceName -like '*engtype_copy' }
    $u3=($e3 | Measure-Object CookedValue -Sum).Sum; $uc=($ec | Measure-Object CookedValue -Sum).Sum
    $top=$e3 | Sort-Object CookedValue -Descending | Select-Object -First 1
    $ded=($mine | Where-Object { $_.Path -like '*dedicated usage' } | Measure-Object CookedValue -Sum).Sum
    $sh=($mine | Where-Object { $_.Path -like '*shared usage' } | Measure-Object CookedValue -Sum).Sum
    # dwm_cpu is % of one logical CPU (Process counter), cpu_total % of the machine.
    '{0} 3d {1:N1} copy {2:N1} ded_mb {3:N0} shared_mb {4:N0} dwm_cpu {5:N1} cpu_total {6:N1} top {7}' -f (Get-Date).ToUniversalTime().ToString('HH:mm:ss'),$u3,$uc,($ded/1MB),($sh/1MB),$dwm,$cpu,$top.InstanceName | Add-Content $out
  }catch{ "$((Get-Date).ToUniversalTime().ToString('HH:mm:ss')) error $($_.Exception.Message)" | Add-Content $out }
}
"end $((Get-Date).ToUniversalTime().ToString('o'))" | Add-Content $out
