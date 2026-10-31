# LAB SIDE: rt-inst35 for lab-arm.ps1 = a copy of the installed runtime folder <InstallRoot>\tools\hip, for the tools
# that the llama asset does not carry (llama-perplexity). Lines: one per copied file with its SHA-256 prefix.
param([string]$InstallRoot = 'C:\Program Files\amdgpu-wddm')
$src = Join-Path $InstallRoot 'tools\hip'
$dst = 'C:\BC250\tmp\t35hip\rt-inst35'
if (-not (Test-Path -LiteralPath (Join-Path $src 'amdhip64.dll'))) { "no $src\amdhip64.dll"; exit 2 }
Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item -Path "$src\*" -Destination $dst -Force
Get-ChildItem -LiteralPath $dst -File | ForEach-Object { "rt-inst35 $($_.Name) $((Get-FileHash $_.FullName -Algorithm SHA256).Hash.Substring(0, 8))" }
