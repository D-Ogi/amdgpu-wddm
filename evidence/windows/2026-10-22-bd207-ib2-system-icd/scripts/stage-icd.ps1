# BD-207 lab arm (unit A, Windows PowerShell 5.1): a private Vulkan ICD manifest for one game process. The DLL was
# pushed into -Dir before. The manifest is a copy of the installed system ICD's manifest (the VulkanDriverName of
# the display adapter) with library_path pointing at the DLL in -Dir, so the loader sees the same api_version. The
# system ICD and its registration stay as they are; a run selects this manifest with VK_DRIVER_FILES.
#   stage-icd.ps1 -Dir C:\BC250\tmp\vkroute-cp\icd -Sha256 <hash of vulkan_radeon.dll>
param(
    [Parameter(Mandatory)][string]$Dir,
    [Parameter(Mandatory)][string]$Sha256
)
$ErrorActionPreference = 'Stop'
"utc $([DateTime]::UtcNow.ToString('o'))"
$dll = Join-Path $Dir 'vulkan_radeon.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw "no $dll" }
$hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
"dll $dll $hash $((Get-Item -LiteralPath $dll).Length) bytes"
if ($hash -ne $Sha256.ToUpperInvariant()) { throw "hash $hash is not the expected $Sha256" }

$class = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
$found = @()
foreach ($k in Get-ChildItem -LiteralPath $class -ErrorAction SilentlyContinue) {
    $p = Get-ItemProperty -LiteralPath $k.PSPath -ErrorAction SilentlyContinue
    if ($p -and $p.VulkanDriverName) { $found += [pscustomobject]@{ Key = $k.PSChildName; Desc = $p.DriverDesc; Json = @($p.VulkanDriverName)[0] } }
}
$found | ForEach-Object { "adapter $($_.Key) '$($_.Desc)' VulkanDriverName $($_.Json)" }
$sys = @($found | Where-Object { $_.Json -and (Test-Path -LiteralPath $_.Json) })
if ($sys.Count -ne 1) { throw "expected one installed ICD manifest, found $($sys.Count)" }
$text = [IO.File]::ReadAllText($sys[0].Json)
"system manifest: $($text -replace '\s+', ' ')"
$sysDll = Join-Path (Split-Path -Parent $sys[0].Json) 'vulkan_radeon.dll'
if (Test-Path -LiteralPath $sysDll) { "system dll $sysDll $((Get-FileHash -LiteralPath $sysDll -Algorithm SHA256).Hash)" }

$m = [regex]::Match($text, '"api_version"\s*:\s*"([^"]+)"')
if (-not $m.Success) { throw 'no api_version in the system manifest' }
$manifest = "{`n  `"file_format_version`": `"1.0.0`",`n  `"ICD`": {`n    `"library_path`": `".\\vulkan_radeon.dll`",`n    `"api_version`": `"$($m.Groups[1].Value)`"`n  }`n}`n"
$out = Join-Path $Dir 'radeon_icd.json'
[IO.File]::WriteAllText($out, $manifest, (New-Object Text.UTF8Encoding($false)))
"manifest $out $((Get-FileHash -LiteralPath $out -Algorithm SHA256).Hash)"
Get-Content -LiteralPath $out
