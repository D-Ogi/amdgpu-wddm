# LAB (unit A, elevated SSH, Windows PowerShell 5.1), read only: the VMID pool and ring VM flush lines of the KMD
# summary (bc250kmd_cli log summary only), and the loaded KMD image hash. Run before and after a benchmark run.
$ErrorActionPreference = 'Continue'
"utc $([DateTime]::UtcNow.ToString('o'))"
$cli = Join-Path $env:ProgramFiles 'amdgpu-wddm\tools\bc250kmd_cli.exe'
if (-not (Test-Path -LiteralPath $cli)) { 'no bc250kmd_cli'; exit 0 }
$raw = @(& $cli log summary only 2>&1 | ForEach-Object { [string]$_ })
"cli exit $LASTEXITCODE, $($raw.Count) lines"
$raw | Where-Object { $_ -match 'ring VM flush|VMID pool|node 0|timeout|TDR|fault' }
$svc = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\bc250kmd' -Name ImagePath -ErrorAction SilentlyContinue
if ($svc) {
    $path = $svc.ImagePath -replace '^\\SystemRoot', $env:SystemRoot -replace '^System32', (Join-Path $env:SystemRoot 'System32')
    if (Test-Path -LiteralPath $path) { "image $((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash) $path" } else { "image path $path" }
}
