# One more llama.cpp folder for lab-arm.ps1: llama-<Name> = every file of C:\BC250\tmp\t35hip\inbox-llama-<Name>
# (pushed before). Prints the hash of ggml-hip.dll.
param([Parameter(Mandatory)][ValidatePattern('^[a-z0-9]+$')][string]$Name)
$k = 'C:\BC250\tmp\t35hip'
$in = Join-Path $k "inbox-llama-$Name"
if (-not (Test-Path (Join-Path $in 'ggml-hip.dll'))) { "missing: $in\ggml-hip.dll"; exit 2 }
$d = Join-Path $k "llama-$Name"
if (Test-Path $d) { Remove-Item -LiteralPath $d -Recurse -Force }
New-Item -ItemType Directory -Force $d | Out-Null
Copy-Item (Join-Path $in '*') $d -Force
"llama-$Name ggml-hip $((Get-FileHash (Join-Path $d 'ggml-hip.dll')).Hash) files $((Get-ChildItem $d -File).Count)"
