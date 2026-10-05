# Read-only recovery observation. Do not initialize the GPU or rerun inference.
$ErrorActionPreference='Stop'
"time " + (Get-Date).ToString('s')
"boot " + (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString('s')
Get-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters |
 Select-Object EnableFullWddm,EnableGpuSubmit,EnableGart,EnablePsp,EnableGfx,EnableIh,UnconfirmedStarts,LastStage | Format-List
Get-ScheduledTask -TaskName BC250-M9-Tiny -ErrorAction SilentlyContinue | Select-Object TaskName,State | Format-Table
Get-Process llama*,vkcompute -ErrorAction SilentlyContinue | Select-Object Name,Id,StartTime | Format-Table
$dir='C:\BC250\m9\tiny-gpu-001'
Get-ChildItem $dir -File -ErrorAction SilentlyContinue | Select-Object Name,Length,LastWriteTime | Format-Table
foreach ($name in @('gate.txt','gart.txt','psp.txt','ih.txt','gfx-1.txt','gfx-2.txt','gfx-3.txt','gfx-4.txt','gfx-5.txt','gfx-6.txt','gfx-7.txt','gfx-8.txt','fence-control.txt','suite.err','suite.txt','suite.exit','temperature.txt','restored.txt')) {
 if (Test-Path (Join-Path $dir $name)) {
  "FILE $name"
  Get-Content (Join-Path $dir $name) -Tail 12
 }
}
