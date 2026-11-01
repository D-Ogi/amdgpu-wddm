# Host test of installer\rocm-venvs.ps1: the uninstaller puts AMD's DLLs back into each TheRock venv that
# attach-rocm-venv.ps1 listed, skips a venv that is gone with a log line, and keeps the entry of a venv it cannot change.
#   powershell -NoProfile -File tools\release\test-rocm-venvs.ps1 -Out <folder>
# Works only in a folder under -Out: stand-in venvs, a stand-in payload with the real compute\hip\attach-rocm-venv.ps1,
# and the list folder. It reads and writes nothing in %ProgramData%.
param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'installer\common.ps1')
. (Join-Path $PSScriptRoot 'installer\rocm-venvs.ps1')
$script:checks = 0
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw "FAIL: $Message" }; $script:checks++ }
function Sha([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Write-Bytes([string]$Path, [string]$Text) { [IO.File]::WriteAllBytes($Path, [Text.Encoding]::ASCII.GetBytes($Text)) }
$attach = Join-Path (Split-Path (Split-Path $PSScriptRoot)) 'compute\hip\attach-rocm-venv.ps1'
if (-not (Test-Path -LiteralPath $attach)) { throw "no $attach" }

$base = Join-Path ([IO.Path]::GetFullPath($Out)) ('rocm-venvs-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($base)
$script:LogPath = Join-Path $base 'uninstall.log'
$amdFace = Join-Path $base 'amd-face'
$amdLt = Join-Path $base 'amd-lt'
Write-Bytes $amdFace 'AMD amdhip64_7'
Write-Bytes $amdLt 'AMD libhipblaslt'
# The payload of the install root, and the one of the release before an upgrade.
function New-Payload([string]$Dir, [string]$Tag, [bool]$WithStep = $true) {
    [void][IO.Directory]::CreateDirectory($Dir)
    Write-Bytes (Join-Path $Dir 'amdhip64_7.dll') "our face $Tag"
    Write-Bytes (Join-Path $Dir 'libhipblaslt.dll') "our front $Tag"
    if ($WithStep) { Copy-Item -LiteralPath $attach -Destination $Dir }
    $m = [ordered]@{ schema = 2; therock = '10.3.0a20261030'
        face = [ordered]@{ file = 'amdhip64_7.dll'; amd_sha256 = (Sha $amdFace) }
        blaslt_front = [ordered]@{ file = 'libhipblaslt.dll'; amd_sha256 = (Sha $amdLt); amd_exports = 323; served_names = @('hipblasLtMatmul') } }
    [IO.File]::WriteAllText((Join-Path $Dir 'payload.json'), ($m | ConvertTo-Json -Depth 4))
}
$payload = Join-Path $base 'install-root\tools\hip\rocm'
$payloadOld = Join-Path $base 'release-before\tools\hip\rocm'
New-Payload $payload 'now'
New-Payload $payloadOld 'before'
$list = Join-Path $base 'list'
function New-Venv([string]$Name) {
    $v = Join-Path $base "venvs\$Name"
    foreach ($d in '_rocm_sdk_core\bin', '_rocm_sdk_libraries\bin') { [void][IO.Directory]::CreateDirectory((Join-Path $v "Lib\site-packages\$d")) }
    Copy-Item -LiteralPath $amdFace -Destination (Join-Path $v 'Lib\site-packages\_rocm_sdk_core\bin\amdhip64_7.dll')
    Copy-Item -LiteralPath $amdLt -Destination (Join-Path $v 'Lib\site-packages\_rocm_sdk_libraries\bin\libhipblaslt.dll')
    return $v
}
function Attach([string]$Venv, [string]$From, [string]$Listed = $list) {
    $o = & powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $From 'attach-rocm-venv.ps1') -Venv $Venv -Payload $From -ListDir $Listed 2>&1 | Out-String
    Assert ($LASTEXITCODE -eq 0) "attach of $Venv (exit $LASTEXITCODE): $o"
}
function Face([string]$Venv) { return Join-Path $Venv 'Lib\site-packages\_rocm_sdk_core\bin\amdhip64_7.dll' }
function Front([string]$Venv) { return Join-Path $Venv 'Lib\site-packages\_rocm_sdk_libraries\bin\libhipblaslt.dll' }
# AMD's two DLLs live, no kept copy, no record: the venv as pip installed it.
function Test-Detached([string]$Venv) {
    return ((Sha (Face $Venv)) -eq (Sha $amdFace)) -and ((Sha (Front $Venv)) -eq (Sha $amdLt)) -and
        -not (Test-Path -LiteralPath ((Face $Venv) + '.orig')) -and
        -not (Test-Path -LiteralPath (Join-Path $Venv 'Lib\site-packages\_rocm_sdk_libraries\bin\libhipblaslt_amd.dll')) -and
        -not (Test-Path -LiteralPath (Join-Path $Venv 'bc250-rocm-attach.json'))
}
function Test-Ours([string]$Venv, [string]$From) {
    return ((Sha (Face $Venv)) -eq (Sha (Join-Path $From 'amdhip64_7.dll'))) -and ((Sha (Front $Venv)) -eq (Sha (Join-Path $From 'libhipblaslt.dll')))
}
function Entries([string]$Dir = $list) { return @(Get-ChildItem -LiteralPath $Dir -Filter '*.json' -ErrorAction SilentlyContinue) }
function Result($Rows, [string]$Key) { return @($Rows | Where-Object { $_.venv -eq $Key -or $_.file -eq $Key }) }
function Log { return [IO.File]::ReadAllText($script:LogPath) }

try {
    # No list at all: nothing to do, and it says so.
    $script:DryRunMode = $false
    $r = @(Invoke-RocmVenvDetach -Payload $payload -ListDir (Join-Path $base 'no-list'))
    Assert ($r.Count -eq 0 -and (Log) -match 'no TheRock venv is attached to our HIP runtime') 'no list: no row, one log line'
    Assert (-not (Get-RocmVenvFootprint (Join-Path $base 'no-list')).present) 'no list: the footprint has no venv'

    $a = New-Venv 'a'; Attach $a $payload
    $b = New-Venv 'b'; Attach $b $payload
    $c = New-Venv 'c'; Attach $c $payload
    $d = New-Venv 'd'; Attach $d $payloadOld
    Remove-Item -LiteralPath $b -Recurse -Force
    $garbage = Join-Path $list 'garbage.json'; Write-Bytes $garbage 'not JSON'
    $unc = Join-Path $list 'unc.json'; [IO.File]::WriteAllText($unc, (@{ venv = '\\bc250-no-such-host.invalid\share\venv' } | ConvertTo-Json))
    $empty = Join-Path $list 'empty.json'; [IO.File]::WriteAllText($empty, '{}')
    Assert ((Entries).Count -eq 7) "seven entries before the uninstall ($((Entries).Count))"
    Assert ((Test-Ours $a $payload) -and (Test-Ours $d $payloadOld)) 'a and d hold our DLLs, d those of the release before'

    # A dry run: the attach step runs with -PlanOnly, and nothing changes.
    $script:DryRunMode = $true
    $r = @(Invoke-RocmVenvDetach -Payload $payload -ListDir $list)
    Assert ($r.Count -eq 7) "dry run: one row per entry ($($r.Count))"
    Assert ((Result $r $a)[0].result -eq 'planned' -and (Result $r $b)[0].result -eq 'gone' -and (Result $r $d)[0].result -eq 'planned') 'dry run: a and d planned, b gone'
    Assert ((Result $r $garbage)[0].result -eq 'unreadable' -and (Result $r $unc)[0].result -eq 'unreadable' -and (Result $r $empty)[0].result -eq 'unreadable') 'dry run: the three bad entries are unreadable'
    Assert ((Result $r $unc)[0].detail -match 'not a folder on a local drive letter') 'dry run: a network path is refused before any access'
    Assert ((Entries).Count -eq 7 -and (Test-Ours $a $payload) -and (Test-Ours $c $payload) -and (Test-Ours $d $payloadOld)) 'dry run: no entry and no venv changed'
    $fp = Get-RocmVenvFootprint $list
    Assert ($fp.present -and $fp.known -and $fp.detail -match '7 listed') "dry run: the footprint lists the seven entries ($($fp.detail))"

    # The real run, while a Python of c still holds its front.
    $script:DryRunMode = $false
    $held = [IO.File]::Open((Front $c), 'Open', 'Read', 'Read')
    try { $r = @(Invoke-RocmVenvDetach -Payload $payload -ListDir $list) } finally { $held.Close() }
    Assert ((Result $r $a)[0].result -eq 'detached' -and (Test-Detached $a)) 'a: detached, AMD''s DLLs back'
    Assert ((Result $r $a)[0].detail -match 'DETACHED') "a: the row carries the last line of the attach step ($((Result $r $a)[0].detail))"
    Assert ((Result $r $d)[0].result -eq 'detached' -and (Test-Detached $d)) 'd: attached by the release before, detached by the record in the venv'
    Assert ((Result $r $b)[0].result -eq 'gone' -and (Log) -match ('skipped: the TheRock venv ' + [regex]::Escape($b) + ' does not exist any more')) 'b: gone, skipped with a log line'
    Assert ((Result $r $c)[0].result -eq 'kept' -and (Test-Ours $c $payload) -and (Result $r $c)[0].detail -match 'exit 4') "c: in use, kept with our DLLs ($((Result $r $c)[0].detail))"
    Assert ((Log) -match ('WARNING: the TheRock venv ' + [regex]::Escape($c) + ' keeps our HIP runtime DLLs')) 'c: a warning names the venv'
    Assert ((@(Entries | ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).venv }) -join '|') -eq $c) 'only the entry of c is left'
    Assert (-not (Test-Path -LiteralPath $garbage) -and -not (Test-Path -LiteralPath $unc) -and -not (Test-Path -LiteralPath $empty)) 'the bad entries are removed'
    $fp = Get-RocmVenvFootprint $list
    Assert ($fp.present -and $fp.detail -match [regex]::Escape($c)) "the footprint names c ($($fp.detail))"

    # Python of c closed: the next uninstall run detaches it, and the empty list goes.
    $r = @(Invoke-RocmVenvDetach -Payload $payload -ListDir $list)
    Assert ($r.Count -eq 1 -and $r[0].result -eq 'detached' -and (Test-Detached $c)) 'c: detached on the next run'
    Assert (-not (Test-Path -LiteralPath $list)) 'the empty list folder is removed'
    Assert (-not (Get-RocmVenvFootprint $list).present) 'the footprint has no venv left'

    # An install root without the attach step: the venv is kept, with a warning.
    $list2 = Join-Path $base 'list2'
    $e = New-Venv 'e'; Attach $e $payload $list2
    $r = @(Invoke-RocmVenvDetach -Payload (Join-Path $base 'no-step') -ListDir $list2)
    Assert ($r.Count -eq 1 -and $r[0].result -eq 'kept' -and $r[0].detail -match 'is missing' -and (Test-Ours $e $payload) -and (Entries $list2).Count -eq 1) 'no attach step: kept, entry kept'
} finally {
    $script:DryRunMode = $false
    Remove-Item -LiteralPath $base -Recurse -Force -ErrorAction SilentlyContinue
}
"test-rocm-venvs: $($script:checks) checks passed"
exit 0
