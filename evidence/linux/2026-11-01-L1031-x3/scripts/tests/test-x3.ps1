# Offline: imports only pure policy. Never imports or executes the live probe/wrapper.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
. (Join-Path $PSScriptRoot 'x3-env.ps1')
. (Join-Path $PSScriptRoot 'x3-storage.ps1')
$checks=0
function Check([bool]$Condition,[string]$Name) {
    $script:checks++
    if (-not $Condition) { throw "FAIL $Name" }
}
function Refuses([scriptblock]$Action,[string]$Name) {
    $failed=$false
    try { & $Action | Out-Null } catch { $failed=$true }
    Check $failed $Name
}
function EnvBlock([string]$Lines) {
    $s="# GRUB Environment Block`n" + $Lines
    return ,[Text.Encoding]::ASCII.GetBytes($s + ('#' * (1024-$s.Length)))
}
$other="saved_entry=keep # value`nBC250_try=preserve`nempty=`n"
$result=Clear-X3Env (EnvBlock ($other+"bc250_try=3`n"))
Check ([Convert]::ToBase64String($result) -ceq [Convert]::ToBase64String((EnvBlock $other))) 'unrelated variables preserved exactly'
Check ((Clear-X3Env (EnvBlock '')).Length -eq 1024) 'empty block valid'
foreach ($bad in @("bc250_try=0`n","bc250_try=1`nbc250_try=2`n","x=1`nx=2`n","bad`n","x=1`r`n","x=1")) {
    Refuses { Clear-X3Env (EnvBlock $bad) } "reject malformed env [$bad]"
}
Refuses { Clear-X3Env ([byte[]]@(0,1)) } 'wrong size'
$block=EnvBlock ''; $block[2]=65
Refuses { Clear-X3Env $block } 'wrong header'
$block=EnvBlock ''; $block[100]=0
Refuses { Clear-X3Env $block } 'NUL byte'

$health='health abi=1 version=0x000700D8 flags=15 generation=123 epoch=4 completed=77 age_ms=100 ready_ms=61000'
$h=Read-X3Health $health 0
Check ($h.Generation -eq 123) 'parse exact current KMD snapshot'
$quiet=$health.Replace('flags=15','flags=31').Replace('age_ms=100','age_ms=99999')
Check ((Read-X3Health $quiet 0).Flags -eq 31) 'QUIET idle desktop accepted'
foreach ($bad in @($health.Replace('flags=15','flags=7'),$health.Replace('age_ms=100','age_ms=15001'),
    $health.Replace('completed=77','completed=0'),$health.Replace('ready_ms=61000','ready_ms=59999'),
    $health.Replace('abi=1','abi=2'),$health.Replace('generation=123','generation=0'),
    ($health+"`n"+$health),'flags=15')) {
    Refuses { Read-X3Health $bad 0 } 'invalid/incomplete/stale KMD snapshot'
}
Refuses { Read-X3Health $health 1 } 'native exit propagated'
Refuses { Assert-X3HealthPair $h (Read-X3Health ($health.Replace('epoch=4','epoch=5')) 0) } 'reset during observation'

$boot='2026-11-01T01:08:04.0000000Z'
function Desktop([int]$Cpu) {
    return [pscustomobject]@{Schema=1;BootUtc=$boot;SessionId=2;ShellPid=10;ShellStart=1234;DwmPid=20;DwmStart=1230;
        ForceCpu=$Cpu;Zink=($Cpu -eq 0);CpuRenderer=($Cpu -eq 1);ShellResponsive=$true;DwmFlushed=$true}
}
Check ((Assert-X3Boot (Desktop 0) (Desktop 0) $boot $true $true $h $h) -eq 'gpu') 'healthy GPU boot'
Check ((Assert-X3Boot (Desktop 1) (Desktop 1) $boot $true $true $null $null) -eq 'cpu') 'intentional CPU recovery without GPU confirmation'
$bad=Desktop 0; $bad.ShellResponsive=$false
Refuses { Assert-X3Boot $bad $bad $boot $true $true $h $h } 'GUI broken but SSH alive'
$bad=Desktop 1; $bad.DwmFlushed=$false
Refuses { Assert-X3Boot $bad $bad $boot $true $true $null $null } 'CPU DWM stalled but services alive'
Refuses { Assert-X3Boot $null $null $boot $false $false $null $null } 'true boot failure/no witness'
Refuses { Assert-X3Boot (Desktop 0) (Desktop 0) $boot $true $true $null $null } 'GPU unconfirmed cannot claim CPU recovery'
$bad=Desktop 1; $bad.Zink=$true
Refuses { Assert-X3Boot $bad $bad $boot $true $true $null $null } 'policy changed but GPU DWM still loaded'
$bad=Desktop 1; $bad.CpuRenderer=$false
Refuses { Assert-X3Boot $bad $bad $boot $true $true $null $null } 'CPU renderer not observed'
$bad=Desktop 0; $bad.BootUtc='2026-10-31T01:08:04.0000000Z'
Refuses { Assert-X3Boot $bad $bad $boot $true $true $h $h } 'previous boot witness'
$bad=Desktop 0; $bad.SessionId=0
Refuses { Assert-X3Boot $bad $bad $boot $true $true $h $h } 'session zero'
$bad=Desktop 0; $bad.ShellPid=11
Refuses { Assert-X3Boot (Desktop 0) $bad $boot $true $true $h $h } 'shell restarted between samples'
Refuses { Assert-X3Boot (Desktop 1) (Desktop 1) $boot $false $true $null $null } 'SSHD unavailable'
Refuses { Assert-X3Boot (Desktop 1) (Desktop 1) $boot $true $false $null $null } 'emergency task unavailable'

# Real PS parser on all source scripts, including live code, without executing it.
foreach ($name in @('x3-env.ps1','x3-storage.ps1','x3-clear.ps1','x3-session.ps1','x3-task.ps1','x3-read.ps1')) {
    $tokens=$null; $errors=$null
    [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name), [ref]$tokens, [ref]$errors) | Out-Null
    Check ($errors.Count -eq 0) "PowerShell syntax $name"
}
$testPath = Join-Path $PSScriptRoot ('test-storage-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testPath) | Out-Null
try {
    $envPath = Join-Path $testPath 'grubenv'
    $old = EnvBlock ($other + "bc250_try=2`n")
    [IO.File]::WriteAllBytes($envPath, $old)
    $result = Update-X3Counter $envPath
    Check ($result.StartsWith('cleared; backup=')) 'real file transaction completed'
    Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($envPath)) -ceq [Convert]::ToBase64String((EnvBlock $other))) 'real file preserves variables'
    $backups = @(Get-ChildItem -LiteralPath $testPath -Filter 'grubenv.x3-backup-*')
    Check ($backups.Count -eq 1 -and [Convert]::ToBase64String([IO.File]::ReadAllBytes($backups[0].FullName)) -ceq [Convert]::ToBase64String($old)) 'original bytes retained'
    Check ((Update-X3Counter $envPath) -eq 'already clear') 'idempotent acknowledgement'
    [IO.File]::WriteAllBytes($envPath, $old)
    # Real Windows share lock: reads work; deletion/rename must fail and propagate.
    $lock = [IO.File]::Open($envPath,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try { Refuses { Update-X3Counter $envPath } 'locked env refuses commit' } finally { $lock.Dispose() }
    Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($envPath)) -ceq [Convert]::ToBase64String($old)) 'failed commit retains live bytes'
    [IO.File]::WriteAllBytes($envPath,[byte[]]@(0,1))
    Refuses { Update-X3Counter $envPath } 'corrupt env is not silently replaced'
    Check (([IO.File]::ReadAllBytes($envPath)).Length -eq 2) 'corrupt input retained for recovery'
} finally {
    $resolved = (Resolve-Path -LiteralPath $testPath).Path
    if ([IO.Path]::GetDirectoryName($resolved) -cne (Resolve-Path -LiteralPath $PSScriptRoot).Path) { throw 'test cleanup escaped X3 directory' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
"X3 PowerShell policy/parser: $checks checks PASS. No GUI probe or live operation executed."
