# Make the Ollama component zip of a tester release from a staging tree that build-ollama.ps1 filled.
#
# The zip holds one directory, `ollama`, so the installer unpacks it into <install root>\tools\ollama
# and nothing moves afterwards. The result is one file plus one JSON record: the name, the size and
# the SHA-256 that the installer's components.json carries.
[CmdletBinding()]
param(
    [string]$Staging,
    [string]$OutputDirectory,
    [string]$Train = '27',
    [string]$Version,
    # The release step names the asset after the driver release, because that is the name the
    # installer's components.json carries: bc250-ollama-<driver release>.zip. Without this the zip
    # is named after the Ollama version it holds, which is what a build outside a release wants.
    [string]$ZipName,
    [switch]$AllowMissingBackends
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workspace = 'P:/BC-250'
if (-not $Staging) { $Staging = "$workspace/scratch/llm-components/staging/ollama" }
if (-not $OutputDirectory) { $OutputDirectory = "$workspace/scratch/llm-components/dist" }
$Staging = $Staging.Replace('\', '/')
$OutputDirectory = $OutputDirectory.Replace('\', '/')
$sourceRoot = "$workspace/scratch/llm-components/ollama-src/ollama"

function Assert-Path([string]$path, [string]$what) {
    if (-not (Test-Path $path)) { throw "$what is missing: $path" }
}

Assert-Path "$Staging/ollama.exe" 'ollama.exe'
Assert-Path "$Staging/lib/ollama/llama-server.exe" 'llama-server.exe'

$backends = [ordered]@{
    vulkan      = "$Staging/lib/ollama/vulkan/ggml-vulkan.dll"
    rocm_bc250  = "$Staging/lib/ollama/rocm_bc250/ggml-hip.dll"
}
$present = @()
foreach ($name in $backends.Keys) {
    if (Test-Path $backends[$name]) { $present += $name }
    elseif (-not $AllowMissingBackends) { throw "the $name backend is missing: $($backends[$name])" }
    else { Write-Warning "packaging without the $name backend" }
}

if (-not $Version) {
    $inputs = "$workspace/scratch/llm-components/build/inputs.json"
    Assert-Path $inputs 'the build input record'
    $Version = (Get-Content $inputs -Raw | ConvertFrom-Json).ollama_version
}

# The runtime entry point and the licences travel with the binaries.
Copy-Item "$PSScriptRoot/runtime/bc250-ollama.ps1" $Staging -Force
Copy-Item "$PSScriptRoot/runtime/bc250-ollama.cmd" $Staging -Force
Copy-Item "$PSScriptRoot/runtime/README.md" $Staging -Force
Copy-Item "$PSScriptRoot/runtime/THIRD-PARTY.md" $Staging -Force
Assert-Path "$sourceRoot/LICENSE" "Ollama's licence"
Copy-Item "$sourceRoot/LICENSE" "$Staging/LICENSE" -Force

# Every import of every binary we ship must resolve inside the zip or come from Windows itself.
& "$PSScriptRoot/check-imports.ps1" -Root $Staging
if ($LASTEXITCODE) { throw "the import check refused the staging tree: $LASTEXITCODE" }

New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$zipName = if ($ZipName) { $ZipName } else { "bc250-ollama-$Version.zip" }
if ($zipName -notmatch '\.zip$') { throw "the asset name must end in .zip: $zipName" }
$zipPath = "$OutputDirectory/$zipName"
Remove-Item $zipPath -ErrorAction SilentlyContinue

# One directory inside the zip. Compress-Archive of the staging directory itself gives exactly
# that when the directory is named `ollama`.
$layout = "$workspace/scratch/llm-components/dist/layout"
Remove-Item -Recurse -Force $layout -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$layout/ollama" | Out-Null
Copy-Item "$Staging/*" "$layout/ollama" -Recurse -Force
Compress-Archive -Path "$layout/ollama" -DestinationPath $zipPath -CompressionLevel Optimal

$hash = (Get-FileHash $zipPath -Algorithm SHA256).Hash.ToLower()
$record = [ordered]@{
    id       = 'ollama'
    file     = $zipName
    version  = $Version
    train    = $Train
    size     = (Get-Item $zipPath).Length
    sha256   = $hash
    backends = $present
    utc      = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
}
$record | ConvertTo-Json -Depth 4 | Set-Content "$OutputDirectory/$zipName.json" -Encoding utf8
Remove-Item -Recurse -Force $layout -ErrorAction SilentlyContinue

$record | ConvertTo-Json -Depth 4
