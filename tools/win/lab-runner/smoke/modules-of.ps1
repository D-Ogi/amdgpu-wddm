# LAB (elevated SSH, read-only): the graphics modules loaded in one process (ours, the D3D runtimes, DXGI, Vulkan, C
# runtime), with path and SHA256 prefix, so a session can tell which API and which driver files the game runs on.
param([string]$Image = 'ROTTR')
$p = Get-Process -Name $Image -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { "no $Image process"; exit 2 }
"{0} pid {1} started {2:HH:mm:ss}Z cpu {3:N1} s ws {4:N0} MB" -f $Image, $p.Id, $p.StartTime.ToUniversalTime(), $p.CPU, ($p.WorkingSet64 / 1MB)
$p.Modules | Where-Object { $_.ModuleName -match '^(amdgpu|bc250|d3d1|dxgi|vulkan|msvcp140|vcruntime140|d3d12core|dxil)' } |
    Sort-Object ModuleName | ForEach-Object {
        $h = try { (Get-FileHash -LiteralPath $_.FileName).Hash.Substring(0, 8) } catch { '????????' }
        '  {0,-28} {1} {2}' -f $_.ModuleName, $h, $_.FileName
    }
