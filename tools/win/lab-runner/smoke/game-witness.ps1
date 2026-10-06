# LAB (elevated SSH, read-only): for one process image (default ROTTR), prints PID, start time, CPU seconds,
# working set, main window title and which of our graphics modules it has loaded (with SHA256 of each), plus the
# KMD's one-line DPM status. -Kill stops the process instead (end of a smoke session).
param([string]$Image = 'ROTTR', [switch]$Kill)
$mods = 'd3d12.dll','d3d12core.dll','dxgi.dll','d3d11.dll','amdgpu_wddm_d3d12.dll','amdgpu_wddm_vkd3d.dll',
        'amdgpu_wddm_radv.dll','bc250d3d.dll','bc250d3d_router.dll','amdgpu_wddm_d3d11.dll','amdgpu_wddm_dxvk.dll',
        'vulkan-1.dll'
$p = @(Get-Process -Name $Image -ErrorAction SilentlyContinue)
if (-not $p.Count) { "no $Image process" }
foreach ($x in $p) {
    if ($Kill) { Stop-Process -Id $x.Id -Force; "stopped $Image pid $($x.Id)"; continue }
    "{0} pid {1} start {2:HH:mm:ss}Z cpu {3:N1} s ws {4:N0} MB title '{5}'" -f $Image, $x.Id,
        $x.StartTime.ToUniversalTime(), $x.TotalProcessorTime.TotalSeconds, ($x.WorkingSet64 / 1MB), $x.MainWindowTitle
    try {
        $x.Modules | Where-Object { $mods -contains $_.ModuleName.ToLower() } | ForEach-Object {
            "  {0} {1}" -f $_.FileName, (Get-FileHash $_.FileName).Hash.Substring(0, 8)
        }
    } catch { "  modules unreadable: $($_.Exception.Message)" }
}
if (-not $Kill) { & 'C:\BC250\kmd193\bc250kmd_cli.exe' dpm 2>&1 | Select-Object -Last 1 }
