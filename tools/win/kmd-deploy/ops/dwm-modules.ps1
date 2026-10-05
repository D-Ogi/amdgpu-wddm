# Read-only: graphics-related modules loaded by every DWM process (name, path, SHA256).
$ErrorActionPreference = 'Stop'
@(Get-Process dwm | ForEach-Object {
    [ordered]@{ pid = $_.Id; start = $_.StartTime.ToUniversalTime().ToString('o'); modules = @($_.Modules |
        Where-Object { $_.ModuleName -match 'bc250|amdgpu|vulkan|d3d1|dxgi|dxcore|d3d9' } |
        ForEach-Object { [ordered]@{ name = $_.ModuleName; path = $_.FileName } }) }
}) | ConvertTo-Json -Depth 5
