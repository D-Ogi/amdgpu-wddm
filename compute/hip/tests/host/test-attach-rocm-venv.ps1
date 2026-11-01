# test-attach-rocm-venv.ps1 - attach-rocm-venv.ps1 on a stand-in TheRock venv, in Windows PowerShell 5.1 and in
# PowerShell 7 when each is present.
#
# The stand-in venv has the two directories of TheRock (_rocm_sdk_core\bin, _rocm_sdk_libraries\bin) with small
# files in place of AMD's DLLs; the stand-in payload has two other small files in place of ours. Every case checks
# the exit code and the SHA-256 of every file of the venv after the run. A refusal must change no file.
# -Work is a scratch directory that the test makes and removes (it must not exist).
param(
    [Parameter(Mandatory = $true)][string]$Work
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
if (Test-Path -LiteralPath $Work) { throw "-Work $Work exists; the test makes it and removes it" }
$attach = (Resolve-Path (Join-Path $PSScriptRoot '..\..\attach-rocm-venv.ps1')).Path
$script:fails = 0
$script:passes = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { $script:passes++; Write-Host "PASS $What" } else { $script:fails++; Write-Host "FAIL $What" }
}
function Sha([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Write-Bytes([string]$Path, [string]$Text) {
    [IO.File]::WriteAllBytes($Path, [Text.Encoding]::ASCII.GetBytes($Text))
}

$shells = @()
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (Test-Path -LiteralPath $ps51) { $shells += $ps51 }
$pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
if ($pwsh) { $shells += $pwsh.Source }
if (-not $shells) { throw 'neither powershell.exe nor pwsh.exe is present' }

New-Item -ItemType Directory -Force $Work | Out-Null
try {
    $payload = Join-Path $Work 'payload'
    New-Item -ItemType Directory -Force $payload | Out-Null
    Write-Bytes (Join-Path $payload 'face.dll') 'our face'
    Write-Bytes (Join-Path $payload 'front.dll') 'our front'
    $amdFaceText = 'AMD amdhip64_7'
    $amdLtText = 'AMD libhipblaslt'
    $amdFaceFile = Join-Path $Work 'amd-face'
    $amdLtFile = Join-Path $Work 'amd-lt'
    Write-Bytes $amdFaceFile $amdFaceText
    Write-Bytes $amdLtFile $amdLtText
    $manifest = [ordered]@{
        therock = '10.3.0a20261030'
        face = [ordered]@{ file = 'face.dll'; sha256 = (Sha (Join-Path $payload 'face.dll')); amd_sha256 = (Sha $amdFaceFile) }
        blaslt_front = [ordered]@{ file = 'front.dll'; sha256 = (Sha (Join-Path $payload 'front.dll')); amd_sha256 = (Sha $amdLtFile)
            amd_exports = 323; served_names = @('hipblasLtMatmul') }
    }
    [IO.File]::WriteAllText((Join-Path $payload 'payload.json'), ($manifest | ConvertTo-Json -Depth 4))
    $oursFace = $manifest.face.sha256
    $oursLt = $manifest.blaslt_front.sha256
    $amdFace = $manifest.face.amd_sha256
    $amdLt = $manifest.blaslt_front.amd_sha256

    foreach ($shell in $shells) {
        $tag = Split-Path -Leaf $shell
        $venv = Join-Path $Work "venv-$tag"
        $core = Join-Path $venv 'Lib\site-packages\_rocm_sdk_core\bin'
        $lib = Join-Path $venv 'Lib\site-packages\_rocm_sdk_libraries\bin'
        $liveFace = Join-Path $core 'amdhip64_7.dll'
        $keptFace = Join-Path $core 'amdhip64_7.dll.orig'
        $liveLt = Join-Path $lib 'libhipblaslt.dll'
        $keptLt = Join-Path $lib 'libhipblaslt_amd.dll'
        $record = Join-Path $venv 'bc250-rocm-attach.json'
        New-Item -ItemType Directory -Force $core, $lib | Out-Null
        Copy-Item $amdFaceFile $liveFace
        Copy-Item $amdLtFile $liveLt

        function Run([string[]]$Extra) {
            $out = & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $attach -Venv $venv -Payload $payload @Extra 2>&1 |
                ForEach-Object { "$_" }
            return [pscustomobject]@{ code = $LASTEXITCODE; text = ($out -join "`n") }
        }
        function State { return '{0}|{1}|{2}|{3}|{4}' -f (Sha $liveFace), (Sha $keptFace), (Sha $liveLt), (Sha $keptLt), (Test-Path -LiteralPath $record) }
        $detached = '{0}||{1}||False' -f $amdFace, $amdLt
        $attached = '{0}|{1}|{2}|{3}|True' -f $oursFace, $amdFace, $oursLt, $amdLt

        Check ((State) -eq $detached) "${tag}: the stand-in venv starts detached"
        $r = Run @('-Check')
        Check ($r.code -eq 0 -and $r.text -match 'CHECK detached') "${tag}: -Check on a detached venv says detached (exit $($r.code))"
        $r = Run @('-PlanOnly')
        Check ($r.code -eq 0 -and (State) -eq $detached) "${tag}: -PlanOnly changes nothing (exit $($r.code))"

        $r = Run @()
        Check ($r.code -eq 0 -and $r.text -match 'ATTACHED' -and (State) -eq $attached) "${tag}: attach (exit $($r.code))"
        $rec = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json
        Check ($rec.face.sha256 -eq $oursFace -and $rec.blaslt_front.amd_sha256 -eq $amdLt -and $rec.therock -eq '10.3.0a20261030') "${tag}: the record names the four hashes and the build"
        $r = Run @()
        Check ($r.code -eq 0 -and $r.text -match 'already attached' -and (State) -eq $attached) "${tag}: attach again is a no-op (exit $($r.code))"
        $r = Run @('-Check')
        Check ($r.code -eq 0 -and $r.text -match 'CHECK attached') "${tag}: -Check on an attached venv (exit $($r.code))"

        # pip install --force-reinstall of the same rocm_sdk_libraries: AMD's file over ours, the kept copy stays.
        Copy-Item $amdLtFile $liveLt -Force
        $r = Run @('-Check')
        Check ($r.code -eq 3 -and $r.text -match 'partly attached') "${tag}: -Check after a reinstall says partly attached (exit $($r.code))"
        $r = Run @()
        Check ($r.code -eq 0 -and (State) -eq $attached) "${tag}: attach after a reinstall puts ours back (exit $($r.code))"

        # A process holds our front: nothing changes.
        $before = State
        $held = [IO.File]::Open($liveLt, 'Open', 'Read', 'Read')
        try { $r = Run @('-Detach') } finally { $held.Close() }
        Check ($r.code -eq 4 -and $r.text -match 'IN USE' -and (State) -eq $before) "${tag}: -Detach with the front in use is refused and changes nothing (exit $($r.code))"

        $r = Run @('-Detach')
        Check ($r.code -eq 0 -and $r.text -match 'DETACHED' -and (State) -eq $detached) "${tag}: -Detach (exit $($r.code))"
        $r = Run @('-Detach')
        Check ($r.code -eq 0 -and (State) -eq $detached) "${tag}: -Detach again is a no-op (exit $($r.code))"

        # A reinstall then a detach: the kept copy goes, AMD's stays live.
        $r = Run @()
        Copy-Item $amdFaceFile $liveFace -Force
        $r = Run @('-Detach')
        Check ($r.code -eq 0 -and (State) -eq $detached) "${tag}: -Detach after a reinstall of rocm_sdk_core (exit $($r.code))"

        # Another TheRock build: refused by every mode, nothing changes.
        Write-Bytes $liveLt 'AMD libhipblaslt of another nightly'
        $before = State
        foreach ($mode in @(@(), @('-Detach'))) {
            $r = Run $mode
            Check ($r.code -eq 3 -and $r.text -match 'TheRock 10\.3\.0a20261030' -and (State) -eq $before) "${tag}: $(if ($mode) { $mode[0] } else { 'attach' }) with another AMD build is refused and changes nothing (exit $($r.code))"
        }
        $r = Run @('-Check')
        Check ($r.code -eq 3 -and $r.text -match 'not the one this payload names') "${tag}: -Check names another AMD build (exit $($r.code))"
        Copy-Item $amdLtFile $liveLt -Force

        # A kit3 venv: the face attached by lab-setup.ps1, the front not yet.
        Copy-Item $liveFace $keptFace
        Copy-Item (Join-Path $payload 'face.dll') $liveFace -Force
        $r = Run @('-Check')
        Check ($r.code -eq 3 -and $r.text -match 'partly attached') "${tag}: -Check on a kit3 venv says partly attached (exit $($r.code))"
        $r = Run @()
        Check ($r.code -eq 0 -and $r.text -match 'HIP runtime face: already attached' -and (State) -eq $attached) "${tag}: attach of a kit3 venv adds the front (exit $($r.code))"
        $r = Run @('-Detach')

        # A payload file that is not the one payload.json names.
        Write-Bytes (Join-Path $payload 'front.dll') 'a front of another build'
        $r = Run @()
        Check ($r.code -eq 3 -and (State) -eq $detached) "${tag}: attach with a changed payload file is refused (exit $($r.code))"
        Write-Bytes (Join-Path $payload 'front.dll') 'our front'

        # Not a TheRock venv.
        $out = & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $attach -Venv (Join-Path $Work 'nothing') -Payload $payload 2>&1
        Check ($LASTEXITCODE -eq 2) "${tag}: a directory without TheRock is exit 2 (exit $LASTEXITCODE)"
    }
} finally {
    Remove-Item -LiteralPath $Work -Recurse -Force
}
Write-Host "test-attach-rocm-venv: $script:passes PASS, $script:fails FAIL"
if ($script:fails) { exit 1 }
exit 0
