$ErrorActionPreference='Stop'
. "$PSScriptRoot\verify-cpu.ps1"
$b=Get-Content C:\BC250\m13\kmd169-control166-001\baseline.json -Raw|ConvertFrom-Json
$class='HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+(Get-PnpDeviceProperty -InstanceId $b.instance -KeyName DEVPKEY_Device_Driver).Data
$reg='HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters'
$watch=[Diagnostics.Stopwatch]::StartNew()
   $readCpu={
   $registration=Get-ItemProperty $class
   $parameters=Get-ItemProperty $reg
   return @{
    umd_registration=@($registration.UserModeDriverName)
    icd_registration=@($registration.VulkanDriverName)
    parameters=(Select-KmdCpuParameters $parameters)
    dwm=@(Get-Process dwm | ForEach-Object {
     @{pid=$_.Id;start=$_.StartTime.ToUniversalTime().ToString('o');modules=@($_.Modules |
      Where-Object {$_.ModuleName -match 'bc250|vulkan_radeon'} |
      ForEach-Object {@{name=$_.ModuleName;sha256=(Get-FileHash -LiteralPath $_.FileName).Hash}})}
    })
   }
   }

$readHealth={
 $text=& C:\BC250\m9\candidate07147\client\bc250kmd_cli.exe health read|Out-String
 if($LASTEXITCODE -ne 0){throw 'Health read failed'}
 Get-KmdReadyHealth $text '0x000700A6'
}
$ready=Wait-KmdCpuBaseline -Saved $b -Read $readCpu -ReadHealth $readHealth -Deadline ([Diagnostics.Stopwatch]::GetTimestamp()+10*[Diagnostics.Stopwatch]::Frequency)
$readinessJson=$ready|ConvertTo-Json -Depth 10
[IO.File]::WriteAllText("$PSScriptRoot\readiness.json",$readinessJson)
$cpuJson=$ready.observed|ConvertTo-Json -Depth 8
[IO.File]::WriteAllText("$PSScriptRoot\cpu.json",$cpuJson)
@{seconds=$watch.Elapsed.TotalSeconds;readiness_bytes=[Text.Encoding]::UTF8.GetByteCount($readinessJson);cpu_bytes=[Text.Encoding]::UTF8.GetByteCount($cpuJson);attempts=$ready.attempts;health=$ready.health;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json -Depth 5
