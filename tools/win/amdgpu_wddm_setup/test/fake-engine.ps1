# A scripted stand-in for installer\install.ps1, for the setup window's build gate (build.ps1 copies it to
# obj\fake-package\installer\install.ps1): it writes events and a terminal result in the engine's format
# (docs/gui/interfaces-setup.md) for one scenario and changes nothing. Windows PowerShell 5.1 syntax only.
param(
    [switch]$Gui,
    [string]$InvocationId,
    [string]$EventsFile,
    [string]$ResultFile,
    [ValidateSet('plan', 'cancel', 'stale', 'crash', 'restart', 'refused', 'noise')][string]$Scenario = 'plan',
    [Parameter(ValueFromRemainingArguments = $true)]$Rest
)
$ErrorActionPreference = 'Stop'
$script:seq = 0
$utf8 = New-Object Text.UTF8Encoding $false
function Ev([string]$Type, [hashtable]$Data = @{}, [string]$Invocation = $InvocationId) {
    $script:seq++
    $e = [ordered]@{ schema = 'amdgpu-wddm.engine-event/1'; invocation = $Invocation; seq = $script:seq; utc = [DateTime]::UtcNow.ToString('o'); type = $Type }
    foreach ($k in $Data.Keys) { $e[$k] = $Data[$k] }
    [IO.File]::AppendAllText($EventsFile, (($e | ConvertTo-Json -Compress -Depth 6) + "`n"), $utf8)
}
function Result([string]$Outcome, [int]$Code, [string]$Message, [bool]$Mutated, [hashtable]$Extra = @{}, [string]$Invocation = $InvocationId) {
    $r = [ordered]@{ schema = 'amdgpu-wddm.engine-result/1'; invocation = $Invocation; engine = [ordered]@{ contract = 'amdgpu-wddm.engine/1'; package_version = '9.9.9-fake' }
        mode = $(if ($Scenario -eq 'plan' -or $Scenario -eq 'noise') { 'plan' } else { 'run' }); dry_run = $false; action = 'install'; outcome = $Outcome; exit_code = $Code
        mutated = $Mutated; nothing_changed = -not $Mutated; restart = [ordered]@{ required = $false }; consents_needed = @(); failed_checks = @(); message_id = $Message; step = $null; detail = 'fake'; log = $null }
    foreach ($k in $Extra.Keys) { $r[$k] = $Extra[$k] }
    Ev 'result' @{ outcome = $Outcome; exit_code = $Code; message_id = $Message; mutated = $Mutated }
    [IO.File]::WriteAllText($ResultFile, ($r | ConvertTo-Json -Depth 6), $utf8)
    exit $Code
}
if (-not $Gui -or -not $InvocationId -or -not $EventsFile -or -not $ResultFile) { exit 64 }
Write-Host "fake engine: $Scenario"
Ev 'start' @{ mode = 'run'; gui = $true; dry_run = $false; package = (Split-Path (Split-Path $MyInvocation.MyCommand.Path)); contract = 'amdgpu-wddm.engine/1'; phase = $null }
Ev 'stage' @{ id = 'preflight'; text = 'Preflight' }
switch ($Scenario) {
    { $_ -in 'plan', 'noise' } {
        if ($Scenario -eq 'noise') {
            Ev 'stage' @{ id = 'files'; text = 'x' } 'another-run'
            [IO.File]::AppendAllText($EventsFile, "{broken`n", $utf8)
        }
        foreach ($c in @(@('package.ok', 'ok'), @('gpu.ok', 'ok'), @('bitlocker.unknown', 'warn'), @('future.check', 'ok'))) { Ev 'check' @{ id = $c[0]; result = $c[1]; name = 'n'; detail = "$env:USERPROFILE\x on $env:COMPUTERNAME, user $env:USERNAME" } }
        Ev 'decision' @{ action = 'install'; installed_version = $null; package_version = '9.9.9-fake'; phase = 'new'; consents = @('test-signing'); restarts = 2; firmware_source = 'download'; firmware_dir = ''; notes = @('RELEASE-NOTES.md'); compatibility = @{ ok = $true; reasons = @() } }
        Ev 'settings-plan' @{ summary = @{ kept = 1; updated = 0; added = 2; unchanged = 0; command = 0 }; rows = @(
                @{ group = 'parameters'; name = 'DpmMaxMHz'; decision = 'kept'; current = 1700; value = 1500; default = 1500; present = $true },
                @{ group = 'parameters'; name = 'EnableMmio'; decision = 'set'; current = $null; value = 1; default = 1; present = $false },
                @{ group = 'd3d12:witcher3.exe'; name = 'Experiment'; decision = 'set'; current = $null; value = 'a'; default = 'a'; present = $false }) }
        Result 'planned' 0 'result.planned' $false @{ consents_needed = @('test-signing') }
    }
    'cancel' {
        Ev 'cancel' @{ available = $true; where = 'before-changes' }
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while ([DateTime]::UtcNow -lt $deadline -and -not (Test-Path -LiteralPath "$EventsFile.cancel")) { Start-Sleep -Milliseconds 100 }
        Ev 'cancel' @{ available = $false; where = 'before-changes' }
        if (Test-Path -LiteralPath "$EventsFile.cancel") { Result 'cancelled' 8 'result.cancelled' $false }
        Result 'failed' 6 'result.step-failed' $false
    }
    'stale' {
        Ev 'step' @{ description = 'x'; dry_run = $true }
        [IO.File]::WriteAllText($ResultFile, '{"schema":"amdgpu-wddm.engine-result/1","invocation":"someone-else","outcome":"verified","exit_code":0,"message_id":"result.verified"}', $utf8)
        exit 0
    }
    'crash' {
        Ev 'stage' @{ id = 'files'; text = 'Files' }
        Ev 'install-action' @{ action = 'install'; boot_id = 1 }
        Ev 'step' @{ description = 'copy'; dry_run = $false }
        exit 6
    }
    'restart' {
        foreach ($s in 'test-signing') { Ev 'stage' @{ id = $s; text = $s } }
        Ev 'install-action' @{ action = 'install'; boot_id = 1 }
        Ev 'step' @{ description = 'bcdedit'; dry_run = $false }
        Ev 'restart-required' @{ reason_id = 'restart.test-signing'; continuation = 'x' }
        Result 'restart-required' 0 'result.restart-test-signing' $true @{ restart = [ordered]@{ required = $true; reason_id = 'restart.test-signing'; boot_id = 1; continuation = [ordered]@{ kind = 'continue'; command = 'x' } } }
    }
    'refused' {
        Ev 'check' @{ id = 'gpu.missing'; result = 'fail'; name = 'n'; detail = "no device on $env:COMPUTERNAME" }
        Result 'refused' 2 'result.preflight-refused' $false @{ failed_checks = @('gpu.missing') }
    }
}
