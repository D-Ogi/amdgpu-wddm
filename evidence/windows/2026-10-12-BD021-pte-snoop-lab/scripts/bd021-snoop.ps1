# BD-021 reading: the vidmm PTE encoding summary lines (coherent / noncoherent / snoop mismatch per level and
# segment) of every kept KMD stop ring (KeepLog=1) of the last two days, newest first. Read only.
$files = Get-ChildItem -LiteralPath 'C:\BC250\kmdlog' -File -Filter '*stop*.log' -ErrorAction SilentlyContinue |
    Where-Object LastWriteTimeUtc -gt ([DateTime]::UtcNow.AddDays(-2)) | Sort-Object LastWriteTimeUtc -Descending
"stop rings: $($files.Count)"
foreach ($f in $files) {
    $l = Select-String -LiteralPath $f.FullName -Pattern 'vidmm summary: (PTE encoding|segment|level|\d+ cpu-virtual)' |
        ForEach-Object { $_.Line }
    $ver = (Select-String -LiteralPath $f.FullName -Pattern 'version 0\.7\.\d+\.\d+|0\.7\.216\.\d+' | Select-Object -First 1).Line
    "=== $($f.LastWriteTimeUtc.ToString('s'))Z $($f.Name) lines $($l.Count)"
    if ($ver) { "  first version line: $($ver.Trim().Substring(0, [Math]::Min(160, $ver.Trim().Length)))" }
    $l | ForEach-Object { "  $($_.Trim())" }
}
