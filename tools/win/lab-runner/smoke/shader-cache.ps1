# LAB (elevated SSH, read-only): size and newest write of the Mesa/RADV shader disk caches in the lab user's profile
# (counts and sizes only), to see whether a game's pipeline compiles are being cached between runs.
$base = 'C:\Users\bc250\AppData\Local'
$dirs = @(Get-ChildItem $base -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'mesa|radv|dxvk|vkd3d' } | ForEach-Object FullName)
if (-not $dirs.Count) { "no mesa/radv/dxvk/vkd3d cache directory under $base" }
foreach ($d in $dirs) {
    $f = @(Get-ChildItem $d -Recurse -File -ErrorAction SilentlyContinue)
    $newest = ($f | Sort-Object LastWriteTime | Select-Object -Last 1)
    '{0}: {1} files, {2:N1} MB, newest {3}' -f $d, $f.Count, (($f | Measure-Object Length -Sum).Sum / 1MB),
        $(if ($newest) { $newest.LastWriteTime.ToUniversalTime().ToString('HH:mm:ssZ') } else { '-' })
}
