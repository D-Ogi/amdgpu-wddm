$ErrorActionPreference='Stop'
$out='C:\BC250\m9\candidate07136'
if((Invoke-RestMethod http://127.0.0.1:2250/state).stop){throw 'Owner STOP requested'}
foreach($item in (Get-Content (Join-Path $out 'smu136-stage-manifest.json') -Raw | ConvertFrom-Json)){
 if((Get-FileHash -LiteralPath (Join-Path $out $item.relative)).Hash -ne $item.sha256){throw 'Staged hash changed'}
}
if(!(Test-Path "$out\before-handover\SHA256.json")){throw 'Missing backup'}
$native=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters' -ErrorAction Stop).EnableNativeSmu
if($native -eq 1){throw 'Native owner gate already enabled'}
'legacy_retirement_begin='+(Get-Date).ToString('s')
Disable-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV' | Out-Null
Stop-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV'
Disable-ScheduledTask -TaskName 'BC250 monitor overlay' | Out-Null
Stop-ScheduledTask -TaskName 'BC250 monitor overlay'
Start-Sleep -Seconds 2
Get-Process bc250mon -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq 'C:\BC250\mon\bc250mon.exe'} | Stop-Process -Force
if(Get-Process bc250mon -ErrorAction SilentlyContinue){throw 'Monitor process still active; inspect'}
Stop-Service bc250rd -ErrorAction Stop
if((Get-Service bc250rd).Status -ne 'Stopped'){throw 'Legacy reader not stopped'}
'legacy_reader_unloaded'
foreach($name in @('bc250rd.sys','bc250rd_cli.exe','bc250control.dll')){Copy-Item -LiteralPath "$out\reader\$name" -Destination "C:\BC250\bc250rd\$name" -Force}
foreach($name in @('bc250rd.sys','bc250rd_cli.exe','bc250control.dll')){
 if((Get-FileHash "C:\BC250\bc250rd\$name").Hash -ne (Get-FileHash "$out\reader\$name").Hash){throw 'Installed reader copy mismatch'}
}
Start-Service bc250rd
if((Get-Service bc250rd).Status -ne 'Running'){throw 'New reader not running'}
# Read-only legacy GetGfxFrequency request: removed handler must refuse before MMIO.
$env:TEMP='C:\BC250\tmp'; $env:TMP=$env:TEMP
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class LegacySmuProbe {
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr CreateFile(string n,uint a,uint s,IntPtr x,uint c,uint f,IntPtr t);
 [DllImport("kernel32.dll", SetLastError=true)] public static extern bool DeviceIoControl(IntPtr h,uint c,byte[] i,uint n,byte[] o,uint z,out uint r,IntPtr v);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
 public static int Run() {
  IntPtr h=CreateFile(@"\\.\Bc250Rd",0xC0000000u,0,IntPtr.Zero,3,0,IntPtr.Zero);
  if(h==new IntPtr(-1)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
  try { byte[] b=new byte[16];b[0]=0x37; uint n; uint ctl=(0x22u<<16)|(3u<<14)|(0x803u<<2);
   bool ok=DeviceIoControl(h,ctl,b,16,b,16,out n,IntPtr.Zero);
   return ok ? 0 : Marshal.GetLastWin32Error();
  } finally { CloseHandle(h); }
 }
}
"@
$result=[LegacySmuProbe]::Run()
'legacy_smu_win32_result='+$result
if($result -ne 50){throw 'Legacy SMU interface did not return ERROR_NOT_SUPPORTED'}
& C:\BC250\bc250rd\bc250rd_cli.exe temp 1 1
if($LASTEXITCODE -ne 0){throw 'New reader SMN control failed'}
foreach($name in @('bc250mon.exe','bc250control.dll','graphics-modules.json')){Copy-Item -LiteralPath "$out\monitor\$name" -Destination "C:\BC250\mon\$name" -Force}
Enable-ScheduledTask -TaskName 'BC250 monitor overlay' | Out-Null
Start-ScheduledTask -TaskName 'BC250 monitor overlay'
Start-Sleep -Seconds 3
$state=Invoke-RestMethod http://127.0.0.1:2250/state
if($state.stop){throw 'Owner STOP requested'}
$mon=@(Get-Process bc250mon -ErrorAction Stop | Where-Object {$_.Path -eq 'C:\BC250\mon\bc250mon.exe'})
if($mon.Count -ne 1){throw 'Expected one replacement monitor'}
'monitor_pid='+$mon[0].Id
if((Get-ScheduledTask -TaskName 'BC250 GPU clock 1000MHz 820mV').State -ne 'Disabled'){throw 'Legacy task not disabled'}
'legacy_writer_retired='+(Get-Date).ToString('s')
@{Completed=(Get-Date).ToString('s');ReaderSha=(Get-FileHash C:\BC250\bc250rd\bc250rd.sys).Hash;LegacySmuResult=$result;MonitorPid=$mon[0].Id} | ConvertTo-Json | Set-Content "$out\legacy-retired.json"
