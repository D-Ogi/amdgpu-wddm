# Read-only (2026-10-08, after the D3D9On12 run): SHA-256 of the installed x64 D3D12 route files and the kernel driver
# image, boot time and DWM, to pin the stack of the run (no install happened between the run and this read).
$ErrorActionPreference = 'Continue'
$files = @(
    'C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_d3d12.dll',
    'C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_vkd3d.dll',
    'C:\Program Files\amdgpu-wddm\d3d12\amdgpu_wddm_radv.dll',
    'C:\Program Files\amdgpu-wddm\wow64\d3d12\amdgpu_wddm_d3d12.dll',
    'C:\Program Files\amdgpu-wddm\wow64\d3d12\amdgpu_wddm_vkd3d.dll',
    'C:\Program Files\amdgpu-wddm\wow64\d3d12\amdgpu_wddm_radv.dll',
    'C:\Windows\System32\d3d9on12.dll',
    'C:\Windows\SysWOW64\d3d9on12.dll')
foreach ($f in $files) {
    if (Test-Path -LiteralPath $f) { '{0} {1}' -f (Get-FileHash -LiteralPath $f).Hash, $f } else { "ABSENT $f" }
}
$sys = Get-CimInstance Win32_SystemDriver -Filter "Name='bc250kmd'" -ErrorAction SilentlyContinue
if ($sys) { '{0} {1}' -f (Get-FileHash -LiteralPath $sys.PathName.Replace('\??\', '')).Hash, $sys.PathName }
foreach ($f in 'C:\Windows\System32\d3d9on12.dll', 'C:\Windows\SysWOW64\d3d9on12.dll') {
    if (Test-Path -LiteralPath $f) { 'version {0} {1}' -f (Get-Item -LiteralPath $f).VersionInfo.FileVersion, $f }
}
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
"dwm $((Get-Process dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')"
