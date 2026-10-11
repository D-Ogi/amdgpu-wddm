# Start, stop and ask after the Ollama server of this driver.
#
# This script ships inside the Ollama component, next to ollama.exe, and it is the only entry
# point the installer and the control app use. It reads its own location, so it needs no paths
# passed in, and it prints machine-readable answers only: one JSON object from `status`, and an
# exit code from every verb. It never reads a Windows message, so the language of the system
# changes nothing.
#
#   bc250-ollama.ps1 status                       one JSON object, exit 0
#   bc250-ollama.ps1 start                        exit 0 running, 2 the port is taken, 1 failed
#   bc250-ollama.ps1 stop                         exit 0
#   bc250-ollama.ps1 pull -Model gemma4:12b-it-qat  exit 0 pulled, 1 failed
#   bc250-ollama.ps1 register                     exit 0, writes the logon task and the settings
#   bc250-ollama.ps1 unregister                   exit 0, removes them and nothing else
#
# To talk to this server from a command prompt, use bc250-ollama.cmd next to this file instead of
# ollama.exe: it points the client at this server's port and this server's model folder. Bare
# ollama.exe uses the Ollama defaults and would reach another Ollama installation on this machine.
[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('status', 'start', 'stop', 'pull', 'register', 'unregister')]
    [string]$Command = 'status',
    [string]$Model,
    [ValidateSet('auto', 'vulkan', 'rocm_bc250')][string]$Backend,
    [int]$Port,
    [int]$TimeoutSeconds = 60
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$here = $PSScriptRoot
$ollamaExe = Join-Path $here 'ollama.exe'
$stateDir = Join-Path $env:LOCALAPPDATA 'bc250-ollama'
$stateFile = Join-Path $stateDir 'server.json'
$modelsDir = Join-Path $stateDir 'models'
$logFile = Join-Path $stateDir 'server.log'
$taskName = 'BC250 Ollama'
$environmentKey = 'HKCU:\Environment'
$defaultPort = 11434

function Get-Setting([string]$name, [string]$fallback) {
    $item = Get-ItemProperty -Path $environmentKey -Name $name -ErrorAction SilentlyContinue
    if (-not $item -or -not $item.PSObject.Properties[$name]) { return $fallback }
    $value = $item.PSObject.Properties[$name].Value
    if ($null -eq $value -or "$value" -eq '') { return $fallback }
    "$value"
}

function Set-Setting([string]$name, [string]$value) {
    New-ItemProperty -Path $environmentKey -Name $name -Value $value -PropertyType String -Force | Out-Null
}

function Remove-Setting([string]$name) {
    Remove-ItemProperty -Path $environmentKey -Name $name -ErrorAction SilentlyContinue
}

function Get-ChosenBackend {
    if ($Backend) { return $Backend }
    $value = Get-Setting 'BC250_OLLAMA_BACKEND' 'auto'
    if ($value -in @('auto', 'vulkan', 'rocm_bc250')) { return $value }
    'auto'
}

function Get-ChosenPort {
    if ($Port -gt 0) { return $Port }
    $value = Get-Setting 'BC250_OLLAMA_PORT' "$defaultPort"
    $parsed = 0
    if ([int]::TryParse($value, [ref]$parsed) -and $parsed -gt 0 -and $parsed -lt 65536) { return $parsed }
    $defaultPort
}

# The process that holds the listening socket, by object, never by parsed text. Zero means
# nothing listens, and -1 means this Windows cannot answer the question, in which case a plain
# connect attempt still tells us whether the port is busy.
function Get-ListeningProcessId([int]$port) {
    if (Get-Command Get-NetTCPConnection -ErrorAction SilentlyContinue) {
        $listener = Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($listener) { return [int]$listener.OwningProcess }
        return 0
    }
    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.BeginConnect('127.0.0.1', $port, $null, $null)
        if ($connect.AsyncWaitHandle.WaitOne(500) -and $client.Connected) { return -1 }
        return 0
    } catch { return 0 } finally { $client.Close() }
}

function Get-TrackedProcessId {
    if (-not (Test-Path $stateFile)) { return 0 }
    try { return [int](Get-Content $stateFile -Raw | ConvertFrom-Json).pid } catch { return 0 }
}

function Get-Status {
    $port = Get-ChosenPort
    $listening = Get-ListeningProcessId $port
    $tracked = Get-TrackedProcessId
    $trackedAlive = $tracked -gt 0 -and (Get-Process -Id $tracked -ErrorAction SilentlyContinue)
    $ours = $trackedAlive -and ($listening -eq $tracked -or $listening -eq -1)
    [ordered]@{
        running = [bool]$ours
        port    = $port
        pid     = if ($ours) { $tracked } else { 0 }
        backend = Get-ChosenBackend
        foreign = [bool]($listening -ne 0 -and -not $ours)
    }
}

function Set-ServerEnvironment([int]$port) {
    # The server process only. Nothing here is written to the user's environment, so a stock
    # Ollama on the same machine keeps its own settings and its own model store.
    $env:OLLAMA_HOST = "127.0.0.1:$port"
    $env:OLLAMA_MODELS = $modelsDir
    # Our board is the machine's only GPU and reports itself integrated.
    $env:OLLAMA_IGPU_ENABLE = '1'
    # Flash attention on, which this board's HIP and Vulkan backends are built for.
    $env:OLLAMA_FLASH_ATTENTION = '1'
    $backend = Get-ChosenBackend
    if ($backend -eq 'auto') {
        Remove-Item env:OLLAMA_LLM_LIBRARY -ErrorAction SilentlyContinue
    } else {
        $env:OLLAMA_LLM_LIBRARY = $backend
    }
}

function Invoke-Start {
    $status = Get-Status
    if ($status.running) { return 0 }
    if ($status.foreign) {
        Write-Host ("Port {0} is held by another program, so this server was not started. Another Ollama is the " -f $status.port +
                    "usual reason. Stop it, or set BC250_OLLAMA_PORT to a free port and start again.")
        return 2
    }
    New-Item -ItemType Directory -Force $stateDir, $modelsDir | Out-Null
    Set-ServerEnvironment $status.port
    $process = Start-Process -FilePath $ollamaExe -ArgumentList 'serve' -WindowStyle Hidden `
        -RedirectStandardOutput $logFile -RedirectStandardError "$logFile.err" -PassThru
    [ordered]@{ pid = $process.Id; port = $status.port; started = (Get-Date).ToUniversalTime().ToString('o') } |
        ConvertTo-Json | Set-Content $stateFile -Encoding utf8
    # Wait for the socket, not for a log line.
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ((Get-Status).running) { return 0 }
        if (-not (Get-Process -Id $process.Id -ErrorAction SilentlyContinue)) { return 1 }
        Start-Sleep -Milliseconds 400
    }
    1
}

# Every process below one process id, the deepest ones included. Ollama starts the model runner as a
# plain child process, with no job object and no process group, so stopping the server alone would
# leave llama-server.exe holding the model in the board's memory. Its HTTP API has no route that asks
# the server to stop (server/routes.go of the pinned commit lists none), so the process is the handle.
function Get-DescendantProcessIds([int]$id) {
    $found = New-Object System.Collections.Generic.List[int]
    $children = @()
    try {
        $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$id" -ErrorAction SilentlyContinue |
            ForEach-Object { [int]$_.ProcessId })
    } catch { $children = @() }
    foreach ($child in $children) {
        if ($child -eq $id) { continue }
        $found.Add([int]$child)
        foreach ($deeper in (Get-DescendantProcessIds $child)) { $found.Add([int]$deeper) }
    }
    return $found.ToArray()
}

function Stop-OneProcess([int]$id) {
    $process = Get-Process -Id $id -ErrorAction SilentlyContinue
    if (-not $process) { return }
    Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
    $process.WaitForExit(10000) | Out-Null
}

function Invoke-Stop {
    $tracked = Get-TrackedProcessId
    if ($tracked -gt 0) {
        $process = Get-Process -Id $tracked -ErrorAction SilentlyContinue
        if ($process) {
            # The children are listed before the server is stopped: afterwards their parent is gone.
            $children = @(Get-DescendantProcessIds $tracked)
            Stop-OneProcess $tracked
            foreach ($child in $children) { Stop-OneProcess $child }
        }
    }
    # A runner that started after that list was taken, recognised by its own program file inside this
    # folder. A llama-server.exe of another installation is never touched.
    foreach ($left in @(Get-Process -Name 'llama-server' -ErrorAction SilentlyContinue)) {
        $path = ''
        try { $path = [string]$left.Path } catch { $path = '' }
        if ($path -and $path.StartsWith($here, [System.StringComparison]::OrdinalIgnoreCase)) { Stop-OneProcess $left.Id }
    }
    Remove-Item $stateFile -ErrorAction SilentlyContinue
    0
}

function Invoke-Pull {
    if (-not $Model) { return 1 }
    # A pull needs the server. One that this command starts is stopped again afterwards, so a pull from
    # the installer leaves no server running in the account that ran the installation.
    $wasRunning = [bool](Get-Status).running
    if ((Invoke-Start) -ne 0) { return 1 }
    $status = Get-Status
    $env:OLLAMA_HOST = "127.0.0.1:$($status.port)"
    # ollama writes its progress to the error stream, which Windows PowerShell turns into an
    # error record. The exit code is the answer here, not the text.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 1
    try { & $ollamaExe pull $Model 2>&1 | Out-Host; $code = [int]$LASTEXITCODE } finally {
        $ErrorActionPreference = $previous
        if (-not $wasRunning) { Invoke-Stop | Out-Null }
    }
    if ($code) { return 1 }
    0
}

function Invoke-Register {
    New-Item -ItemType Directory -Force $stateDir, $modelsDir | Out-Null
    if (-not (Get-Setting 'BC250_OLLAMA_BACKEND' '')) { Set-Setting 'BC250_OLLAMA_BACKEND' 'auto' }
    if (-not (Get-Setting 'BC250_OLLAMA_PORT' '')) {
        # Leave a stock Ollama on 11434 alone and take the next port.
        $port = if ((Get-ListeningProcessId $defaultPort) -ne 0) { $defaultPort + 1 } else { $defaultPort }
        Set-Setting 'BC250_OLLAMA_PORT' "$port"
    }
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$PSCommandPath`" start"
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
        -StartWhenAvailable -ExecutionTimeLimit ([TimeSpan]::Zero)
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings `
        -Description 'Starts the Ollama server of the BC-250 display driver in this user session.' -Force | Out-Null
    0
}

function Invoke-Unregister {
    Invoke-Stop | Out-Null
    if (Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
    }
    Remove-Setting 'BC250_OLLAMA_BACKEND'
    Remove-Setting 'BC250_OLLAMA_PORT'
    # Our own two files go as well, so the uninstaller's list has nothing of ours to report. The models
    # folder below them stays: those are the user's own files, and the uninstaller's own switch clears it.
    Remove-Item $stateFile, $logFile, "$logFile.err" -Force -ErrorAction SilentlyContinue
    0
}

switch ($Command) {
    'status' { Get-Status | ConvertTo-Json -Compress; exit 0 }
    'start' { exit (Invoke-Start) }
    'stop' { exit (Invoke-Stop) }
    'pull' { exit (Invoke-Pull) }
    'register' { exit (Invoke-Register) }
    'unregister' { exit (Invoke-Unregister) }
}
