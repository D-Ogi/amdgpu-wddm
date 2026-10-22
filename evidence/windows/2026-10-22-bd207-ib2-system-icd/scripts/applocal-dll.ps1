# LAB (elevated SSH, Windows PowerShell 5.1): put app-local DLLs next to a program and take them away again, for a
# route comparison (for example vkd3d-proton d3d12.dll + d3d12core.dll and DXVK dxgi.dll next to a D3D12 game, so
# that it runs D3D12 -> Vulkan on the system Vulkan ICD instead of our D3D12 UMD). Generalized from
# scratch\gh38\vkroute\stage.ps1 (2026-10-20).
#   applocal-dll.ps1 -Action Stage|Restore|Check -Dir <program dir> -Exe <program file name> -Src <dir with the DLLs>
#                    [-Names d3d12.dll,d3d12core.dll,dxgi.dll] [-Journal <json>] [-Clean <file name patterns>]
# Stage refuses when one of -Names already exists in -Dir or when the program runs, and writes the hash of each copy
# into -Journal. Restore removes exactly the files whose hash matches the journal, then the files of -Clean (exact
# patterns, for example the pipeline cache and the log the swapped DLLs leave in -Dir), and lists -Dir.
param(
    [ValidateSet('Stage', 'Restore', 'Check')][string]$Action = 'Check',
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string]$Exe,
    [string]$Src = '',
    [string]$Names = 'd3d12.dll,d3d12core.dll,dxgi.dll',
    [string]$Journal = '',
    [string]$Clean = ''
)
$ErrorActionPreference = 'Stop'
"utc $([DateTime]::UtcNow.ToString('o'))"
if (-not (Test-Path -LiteralPath (Join-Path $Dir $Exe))) { throw "$Exe not found in $Dir" }
if (-not $Journal) { $Journal = Join-Path 'C:\BC250\tmp\applocal' (($Exe -replace '\.exe$', '') + '.journal.json') }
$nameList = @($Names -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$procName = [IO.Path]::GetFileNameWithoutExtension($Exe)
if ($Action -ne 'Check' -and @(Get-Process -Name $procName -ErrorAction SilentlyContinue).Count) { throw "$procName runs; close it first" }
switch ($Action) {
    'Stage' {
        if (-not $Src) { throw '-Src is required for Stage' }
        if (Test-Path -LiteralPath $Journal) { throw "journal $Journal exists: an earlier stage was not restored" }
        foreach ($n in $nameList) { if (Test-Path -LiteralPath (Join-Path $Dir $n)) { throw "$n already exists in $Dir; refusing" } }
        foreach ($n in $nameList) { if (-not (Test-Path -LiteralPath (Join-Path $Src $n))) { throw "missing source $(Join-Path $Src $n)" } }
        New-Item -ItemType Directory -Force (Split-Path -Parent $Journal) | Out-Null
        $rows = foreach ($n in $nameList) {
            Copy-Item -LiteralPath (Join-Path $Src $n) -Destination (Join-Path $Dir $n)
            [ordered]@{ name = $n; sha256 = (Get-FileHash -LiteralPath (Join-Path $Dir $n) -Algorithm SHA256).Hash }
        }
        $j = [ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); dir = $Dir; exe = $Exe; files = @($rows) }
        [IO.File]::WriteAllText($Journal, ($j | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
        "staged: $(($rows | ForEach-Object { "$($_.name) $($_.sha256.Substring(0,8))" }) -join ', ')"
    }
    'Restore' {
        if (-not (Test-Path -LiteralPath $Journal)) { throw "no journal at $Journal" }
        $j = Get-Content -LiteralPath $Journal -Raw | ConvertFrom-Json
        if ($j.dir -ne $Dir) { throw "the journal is for $($j.dir), not $Dir" }
        foreach ($f in $j.files) {
            $p = Join-Path $Dir $f.name
            if (-not (Test-Path -LiteralPath $p)) { "absent: $($f.name)"; continue }
            $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
            if ($h -ne $f.sha256) { "KEPT (hash differs, not ours): $($f.name) $h"; continue }
            Remove-Item -LiteralPath $p
            "removed: $($f.name)"
        }
        foreach ($pat in @($Clean -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
            Get-ChildItem -LiteralPath $Dir -File -Filter $pat | ForEach-Object { Remove-Item -LiteralPath $_.FullName; "removed: $($_.Name) $($_.Length)" }
        }
        Rename-Item -LiteralPath $Journal -NewName ("{0}-restored-{1}.json" -f [IO.Path]::GetFileNameWithoutExtension($Journal), [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ'))
    }
    'Check' { if (Test-Path -LiteralPath $Journal) { "journal $Journal" } else { "no journal at $Journal" } }
}
foreach ($n in $nameList) {
    $p = Join-Path $Dir $n
    if (Test-Path -LiteralPath $p) { "  present $n $((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.Substring(0,8))" } else { "  absent $n" }
}
Get-ChildItem -LiteralPath $Dir -File | Where-Object { $_.Name -match '\.(cache|write|log)$|cache\.write' } | ForEach-Object { "  extra $($_.Name) $($_.Length)" }
