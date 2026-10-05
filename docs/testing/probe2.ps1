# Read-only: dxdiag /t into C:\BC250\tmp, print only the display-device and DirectX-feature lines (no machine/user ids).
$ErrorActionPreference = 'Continue'
$out = 'C:\BC250\tmp\defaults-audit-dxdiag.txt'
Remove-Item -LiteralPath $out -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$env:windir\System32\dxdiag.exe" -ArgumentList '/t', $out -PassThru -WindowStyle Hidden
if (-not $p.WaitForExit(120000)) { 'dxdiag did not finish in 120 s'; try { $p.Kill() } catch { }; exit 1 }
for ($i = 0; $i -lt 20 -and -not (Test-Path -LiteralPath $out); $i++) { Start-Sleep -Milliseconds 500 }
if (-not (Test-Path -LiteralPath $out)) { 'no dxdiag output'; exit 1 }
$keep = 'DirectX Version|DxDiag Version|Card name|Manufacturer|Chip type|Display Memory|Dedicated Memory|Shared Memory|Current Mode|HDR Support|Display Color Space|Monitor Name|Monitor Model|Native Mode|Output Type|Advanced Color|Driver Name|Driver File Version|Driver Version|DDI Version|Feature Levels|Driver Model|Hardware Scheduling|Graphics Preemption|Compute Preemption|Miracast|Detachable GPU|Hybrid Graphics|Power P-states|Virtualization|Block List|Catalog Attributes|Driver Attributes|WDDM|DirectDraw Acceleration|Direct3D Acceleration|AGP Texture|DirectML|D3D9 Overlay|DXVA-HD|DXVA2 Modes|Deinterlace|Variable Refresh|MPO|Monitor Capabilities|Display Tile|Notes|User Mode Driver|Kernel Mode Driver|Vulkan|OpenGL'
$lines = Get-Content -LiteralPath $out
$section = ''
foreach ($l in $lines) {
    if ($l -match '^-{5,}') { continue }
    if ($l -match '^(Display Devices|Sound Devices|System Information|DirectX Debug Levels|Disk & DVD|System Devices|DirectShow|MF File|EVR|Diagnostics)') { $section = $Matches[1]; "## $section"; continue }
    if ($section -eq 'Display Devices' -and $l -match "^\s*($keep)\s*:") { $l.TrimEnd() }
    elseif ($section -eq 'System Information' -and $l -match '^\s*(DirectX Version|DxDiag Version|Operating System)\s*:') { $l.TrimEnd() }
    elseif ($section -eq 'DirectX Debug Levels' -and $l -match 'Direct3D|DirectDraw') { $l.TrimEnd() }
}
