# E36 fork-branch control 1: E14 compute smoke on the consolidated RADV ICD.
# Swaps the candidate into the registered ICD path for the bounded smoke (as smoke007 did), restores the
# baseline in finally with a hash check. Uses tools/quality/run-smoke.ps1 unchanged.
$ErrorActionPreference = 'Stop'
$dir = 'C:\BC250\m13\fork-consolidated001'
$active = 'C:\BC250\m10\wsi-final\vulkan_radeon.dll'
$baselineHash = '9C40083C08210C71A6F6DC7EA12F7C1205BBC6AA1DFE48E596C8495B6C2A9798'
$candidate = "$dir\vulkan_radeon.dll"
$candidateHash = 'FAD08ECB16C9CFFDCB8D09D408C8ABC428B348D948D945D78DE30892D88A65AF'
$backup = "$dir\baseline-9C40083C.dll"
$out = "$dir\smoke001"
if ((Invoke-RestMethod http://127.0.0.1:2250/flags -TimeoutSec 3).stop) { throw 'Owner STOP' }
if ((Get-FileHash -LiteralPath $active).Hash -ne $baselineHash) { throw 'Unexpected baseline ICD' }
if ((Get-FileHash -LiteralPath $candidate).Hash -ne $candidateHash) { throw 'Unexpected candidate ICD' }
if (Test-Path $out) { throw 'Result directory exists' }
$raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
if ($raw -notmatch 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { throw 'No temperature' }
"temperature_before=$($Matches[1])"
if ([double]$Matches[1] -ge 85) { throw 'Temperature limit' }
Copy-Item -LiteralPath $active -Destination $backup -Force
if ((Get-FileHash -LiteralPath $backup).Hash -ne $baselineHash) { throw 'Backup mismatch' }
$config = @{
  exe = 'C:\BC250\m8\vkcompute.exe'
  arguments = 'C:\BC250\m8\spv --runs 1'
  timeout_seconds = 90
  pass_pattern = '(?m)^8 test\(s\) run, 0 mismatch\(es\)\s*$'
  modules = @(@{ name = 'vulkan_radeon.dll'; path = $active; sha256 = $candidateHash })
} | ConvertTo-Json -Depth 4
Set-Content -LiteralPath "$dir\smoke001-config.json" -Value $config -Encoding ASCII
$code = 125
try {
  Copy-Item -LiteralPath $candidate -Destination $active -Force
  if ((Get-FileHash -LiteralPath $active).Hash -ne $candidateHash) { throw 'Candidate swap mismatch' }
  'candidate in registered path'
  & "$dir\run-smoke.ps1" -Config "$dir\smoke001-config.json" -Out $out
  $code = 0
} catch {
  "SMOKE_ERROR $($_.Exception.Message)"
  $code = 1
} finally {
  Copy-Item -LiteralPath $backup -Destination $active -Force
  $restored = (Get-FileHash -LiteralPath $active).Hash
  "restored=$restored"
  if ($restored -ne $baselineHash) { "RESTORE_MISMATCH"; $code = 2 }
  $raw = & 'C:\BC250\bc250rd\bc250rd_cli.exe' temp 1 1 | Out-String
  if ($raw -match 'Tctl\s+([0-9]+(?:\.[0-9]+)?)') { "temperature_after=$($Matches[1])" }
}
if (Test-Path "$out\receipt.json") { Get-Content "$out\receipt.json" }
if (Test-Path "$out\stdout.txt") { Get-Content "$out\stdout.txt" | Select-Object -Last 14 }
"smoke_exit=$code"
exit $code
