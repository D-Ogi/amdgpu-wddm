# Read-only: identities of the deployed stack for the W3 RT effect-cost series (KMD image, release files, RADV
# perftest marker, W3 RT keys, CU mode). Changes nothing.
$ErrorActionPreference = 'Continue'
$svc = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -ErrorAction SilentlyContinue
if ($svc) {
    $img = [string]$svc.ImagePath
    $img = $img -replace '^\\SystemRoot', $env:SystemRoot -replace '^System32', (Join-Path $env:SystemRoot 'System32') -replace '^\\\?\?\\', ''
    "kmd image $img"
    if (Test-Path -LiteralPath $img) { "kmd sys $((Get-FileHash -LiteralPath $img).Hash)" }
}
Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceClass='DISPLAY'" | ForEach-Object { "display driver $($_.DeviceName) version $($_.DriverVersion) inf $($_.InfName)" }
$root = 'C:\Program Files\amdgpu-wddm'
foreach ($rel in 'desktop\bc250d3d_router.dll', 'desktop\bc250d3d_zink.dll', 'desktop\amdgpu_wddm_radv.dll', 'desktop\bc250d3d.dll',
    'd3d12\amdgpu_wddm_d3d12.dll', 'd3d12\amdgpu_wddm_vkd3d.dll', 'd3d12\amdgpu_wddm_radv.dll', 'vulkan\vulkan_radeon.dll',
    'tools\bc250kmd_cli.exe', 'd3d11\amdgpu_wddm_d3d11.dll') {
    $p = Join-Path $root $rel
    if (Test-Path -LiteralPath $p) { "file $rel $((Get-FileHash -LiteralPath $p).Hash)" } else { "file $rel ABSENT" }
}
"radv-perftest marker: $(if (Test-Path 'C:\BC250\tools\radv-perftest.txt') { (Get-Content 'C:\BC250\tools\radv-perftest.txt' -Raw).Trim() } else { 'absent' })"
"pso-log.off: $(Test-Path 'C:\BC250\tools\pso-log.off')  shader-dump.on: $(Test-Path 'C:\BC250\tools\shader-dump.on')"
"summary pause: $(Test-Path 'C:\BC250\mon\graphics-summary.pause')"
$f = Join-Path $env:USERPROFILE 'Documents\The Witcher 3\dx12user.settings'
$t = [IO.File]::ReadAllText($f)
[regex]::Matches($t, '(?m)^(Resolution|FullScreenMode|VSync|LimitFPS|EnableRT|EnableRtRadiance|Shadows|RTAOEnabled|RTGIPreset|PTEnable|AAMode)=[^\r\n]*') | ForEach-Object { "w3 $($_.Value)" }
$app = Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\D3D12\Applications\witcher3.exe' -ErrorAction SilentlyContinue
if ($app) { $app.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { "w3 profile $($_.Name)=$($_.Value)" } }
Get-Process witcher3, REDprelauncher -ErrorAction SilentlyContinue | ForEach-Object { "RUNNING $($_.Name) $($_.Id)" }
"boot $((Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o'))"
