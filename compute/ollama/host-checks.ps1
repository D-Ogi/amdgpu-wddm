# Check a staged Ollama component on a machine that has no BC-250 board.
#
# Four checks, all bounded, none of them touching a GPU:
#
#   1. ollama.exe prints a version, and the version names this driver's train.
#   2. llama-server.exe starts, answers and exits.
#   3. The server starts on a free port on the processor backend, answers its own API, and stops.
#      Nothing is left running, the model runner included: the stop runs on every exit path,
#      including a failed check.
#   4. Every import of every binary in the tree resolves.
#
# The GPU backends cannot be checked here. Only the lab can say whether they load, and the lab
# plan of this work says how.
[CmdletBinding()]
param(
    [string]$Staging,
    [int]$Port = 21434,
    [int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not $Staging) { $Staging = 'P:/BC-250/scratch/llm-components/staging/ollama' }
$Staging = (Resolve-Path $Staging).Path
$ollamaExe = Join-Path $Staging 'ollama.exe'
$serverExe = Join-Path $Staging 'lib/ollama/llama-server.exe'

$results = [System.Collections.Generic.List[object]]::new()
function Add-Result([string]$name, [bool]$pass, [string]$detail) {
    $results.Add([pscustomobject]@{ check = $name; pass = $pass; detail = $detail })
    Write-Host ("{0}  {1}  {2}" -f $(if ($pass) { 'PASS' } else { 'FAIL' }), $name, $detail)
}

# Both binaries print their version to the error stream. Windows PowerShell turns that into an
# error record, and with Stop set it ends the script, so the output of a native command is read
# with its own error preference and the exit code is returned next to the text.
function Get-NativeOutput([string]$path, [string[]]$arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = (& $path @arguments 2>&1 | Out-String).Trim()
        return [pscustomobject]@{ text = $text; exit = $LASTEXITCODE }
    } finally { $ErrorActionPreference = $previous }
}

# ---- 1. the version of ollama.exe
$version = Get-NativeOutput $ollamaExe @('--version')
Add-Result 'ollama.exe prints a version' ($version.text -match 'version is \S+') $version.text
Add-Result 'the version names this driver' ($version.text -match 'bc250\.\d+') $version.text

# ---- 2. the runner
$serverVersion = Get-NativeOutput $serverExe @('--version')
$firstLine = ($serverVersion.text -split "`r?`n" | Where-Object { $_ -ne '' } | Select-Object -First 1)
Add-Result 'llama-server.exe starts and exits' ($serverVersion.text -match 'version:') $firstLine

# ---- 3. the server, on the processor backend, started and stopped by this script
$process = $null
try {
    $env:OLLAMA_HOST = "127.0.0.1:$Port"
    $env:OLLAMA_MODELS = Join-Path $env:TEMP 'bc250-ollama-host-check-models'
    # A backend name no directory carries. Discovery then skips every GPU directory, so this
    # check never initialises the GPU of the machine it runs on.
    $env:OLLAMA_LLM_LIBRARY = 'host-check-processor-only'
    New-Item -ItemType Directory -Force $env:OLLAMA_MODELS | Out-Null
    $log = Join-Path $env:TEMP 'bc250-ollama-host-check.log'
    $process = Start-Process -FilePath $ollamaExe -ArgumentList 'serve' -WindowStyle Hidden `
        -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $answer = $null
    while ((Get-Date) -lt $deadline -and -not $answer) {
        try { $answer = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/version" -TimeoutSec 3 } catch { Start-Sleep -Milliseconds 500 }
        if ($process.HasExited) { break }
    }
    Add-Result 'the server answers its own API' ($null -ne $answer) ($answer | ConvertTo-Json -Compress)

    # The device list the server reports. On this machine the expected answer is no GPU, which is
    # what makes the check safe here, and it proves discovery ran instead of crashing.
    $devices = $null
    try { $devices = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/ps" -TimeoutSec 5 } catch { }
    Add-Result 'the server answers after discovery' ($null -ne $devices) ($devices | ConvertTo-Json -Compress)
} catch {
    Add-Result 'the server answers its own API' $false $_.Exception.Message
} finally {
    if ($process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(10000) | Out-Null
    }
    Remove-Item env:OLLAMA_LLM_LIBRARY, env:OLLAMA_HOST, env:OLLAMA_MODELS -ErrorAction SilentlyContinue
    # The model runner is a child process of the server, not a thread of it, so it is counted here as
    # well: a server that is gone while its runner still holds a model is not a stopped server.
    $left = @(Get-Process -Name 'ollama', 'llama-server' -ErrorAction SilentlyContinue)
    Add-Result 'no server and no runner is left running' ($left.Count -eq 0) ("processes: " + $left.Count + " (" + ((@($left | ForEach-Object { $_.ProcessName }) | Sort-Object -Unique) -join ', ') + ")")
}

# ---- 4. the imports
try {
    & "$PSScriptRoot/check-imports.ps1" -Root $Staging | Out-Host
    Add-Result 'every import resolves' ($LASTEXITCODE -eq 0) "exit $LASTEXITCODE"
} catch {
    Add-Result 'every import resolves' $false $_.Exception.Message
}

$failed = @($results | Where-Object { -not $_.pass })
$record = [ordered]@{
    utc     = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    staging = $Staging
    checks  = $results
    failed  = $failed.Count
}
$out = 'P:/BC-250/scratch/llm-components/logs/host-checks.json'
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
$record | ConvertTo-Json -Depth 5 | Set-Content $out -Encoding utf8
Write-Host "record: $out"
exit $failed.Count
