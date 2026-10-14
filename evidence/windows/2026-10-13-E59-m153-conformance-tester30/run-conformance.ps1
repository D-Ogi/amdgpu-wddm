# M15.3/M15.5 rerun on the installed release (2026-10-13, tester.30): the D3D12 conformance client (ROV, conservative
# rasterization tier 3 with inner coverage, indirect DispatchRays) on the BC-250 at FL 12_1, standalone, offscreen.
# The exe is pushed to C:\BC250\tmp\conformance first. Bounded to 150 s by the client's own deadline.
param([string]$Exe = 'C:\BC250\tmp\conformance\amdgpu_wddm_conformance.exe', [int]$Deadline = 150)
$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
$out = "C:\BC250\tmp\conformance\out-$stamp"
New-Item -ItemType Directory -Force -Path $out | Out-Null
'exe sha256 ' + (Get-FileHash -LiteralPath $Exe).Hash
$sw = [Diagnostics.Stopwatch]::StartNew()
$text = & $Exe --adapter bc250 --out $out --deadline $Deadline 2>&1
$rc = $LASTEXITCODE
'exit ' + $rc + ' after ' + [int]$sw.Elapsed.TotalSeconds + ' s, out ' + $out
$text | Select-Object -Last 60
if (Test-Path "$out\conformance.json") { '--- conformance.json'; Get-Content "$out\conformance.json" -Raw }
