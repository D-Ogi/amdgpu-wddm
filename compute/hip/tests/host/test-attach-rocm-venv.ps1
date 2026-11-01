# test-attach-rocm-venv.ps1 - attach-rocm-venv.ps1 on a stand-in TheRock venv, in Windows PowerShell 5.1 and in
# PowerShell 7 when each is present.
#
# The stand-in venv has the two directories of TheRock (_rocm_sdk_core\bin, _rocm_sdk_libraries\bin) with small
# files in place of AMD's DLLs; each stand-in payload has two other small files in place of ours. Every case checks
# the exit code and the SHA-256 of every file of the venv after the run. A refusal must change no file. The list of
# attached venvs (-ListDir) is a directory under -Work, so the test writes nothing to %ProgramData%.
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
# A stand-in payload: two files in place of our DLLs and payload.json. Schema 2 names no hash of ours; schema 1 (the
# kits up to kit-int8-1) names one, and the file must have it.
function New-Payload([string]$Dir, [string]$Face, [string]$Front, [string]$AmdFrontSha, [int]$Schema = 2) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    Write-Bytes (Join-Path $Dir 'face.dll') $Face
    Write-Bytes (Join-Path $Dir 'front.dll') $Front
    $faceEntry = [ordered]@{ file = 'face.dll'; amd_sha256 = $script:amdFace; build_sha256 = (Sha (Join-Path $Dir 'face.dll')) }
    $frontEntry = [ordered]@{ file = 'front.dll'; amd_sha256 = $AmdFrontSha; build_sha256 = (Sha (Join-Path $Dir 'front.dll'))
        amd_exports = 323; served_names = @('hipblasLtMatmul') }
    if ($Schema -eq 1) { $faceEntry['sha256'] = $faceEntry.build_sha256; $frontEntry['sha256'] = $frontEntry.build_sha256 }
    $m = [ordered]@{ schema = $Schema; therock = '10.3.0a20261030'; face = $faceEntry; blaslt_front = $frontEntry }
    [IO.File]::WriteAllText((Join-Path $Dir 'payload.json'), ($m | ConvertTo-Json -Depth 4))
}

$shells = @()
$ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (Test-Path -LiteralPath $ps51) { $shells += $ps51 }
$pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
if ($pwsh) { $shells += $pwsh.Source }
if (-not $shells) { throw 'neither powershell.exe nor pwsh.exe is present' }

New-Item -ItemType Directory -Force $Work | Out-Null
$junctions = @()
try {
    $amdFaceFile = Join-Path $Work 'amd-face'
    $amdLtFile = Join-Path $Work 'amd-lt'
    $amdLtOther = Join-Path $Work 'amd-lt-other'
    Write-Bytes $amdFaceFile 'AMD amdhip64_7'
    Write-Bytes $amdLtFile 'AMD libhipblaslt'
    Write-Bytes $amdLtOther 'AMD libhipblaslt of another nightly'
    $script:amdFace = Sha $amdFaceFile
    $amdLt = Sha $amdLtFile
    $payload = Join-Path $Work 'payload'
    New-Payload $payload 'our face' 'our front' $amdLt
    $payloadB = Join-Path $Work 'payload-b'
    New-Payload $payloadB 'our face of the next release' 'our front of the next release' $amdLt
    $payloadC = Join-Path $Work 'payload-c'
    New-Payload $payloadC 'our face' 'our front for another nightly' (Sha $amdLtOther)
    $payload1 = Join-Path $Work 'payload-schema1'
    New-Payload $payload1 'our face' 'our front' $amdLt 1
    $oursFace = Sha (Join-Path $payload 'face.dll')
    $oursLt = Sha (Join-Path $payload 'front.dll')
    $oursFaceB = Sha (Join-Path $payloadB 'face.dll')
    $oursLtB = Sha (Join-Path $payloadB 'front.dll')

    foreach ($shell in $shells) {
        $tag = Split-Path -Leaf $shell
        $venv = Join-Path $Work "venv-$tag"
        $list = Join-Path $Work "list-$tag"
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

        function Run([string[]]$Extra, [string]$From = $payload, [string]$Target = $venv, [string]$Listed = $list) {
            $out = & $shell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $attach -Venv $Target -Payload $From -ListDir $Listed @Extra 2>&1 |
                ForEach-Object { "$_" }
            return [pscustomobject]@{ code = $LASTEXITCODE; text = ($out -join "`n") }
        }
        function State { return '{0}|{1}|{2}|{3}|{4}' -f (Sha $liveFace), (Sha $keptFace), (Sha $liveLt), (Sha $keptLt), (Test-Path -LiteralPath $record) }
        function Entries { return @(Get-ChildItem -LiteralPath $list -Filter '*.json' -ErrorAction SilentlyContinue) }
        $detached = '{0}||{1}||False' -f $script:amdFace, $amdLt
        $attached = '{0}|{1}|{2}|{3}|True' -f $oursFace, $script:amdFace, $oursLt, $amdLt
        $attachedB = '{0}|{1}|{2}|{3}|True' -f $oursFaceB, $script:amdFace, $oursLtB, $amdLt

        Check ((State) -eq $detached) "${tag}: the stand-in venv starts detached"
        $r = Run @('-Check')
        Check ($r.code -eq 0 -and $r.text -match 'CHECK detached' -and $r.text -match 'not recorded for the uninstaller') "${tag}: -Check on a detached venv says detached and not recorded (exit $($r.code))"
        $r = Run @('-PlanOnly')
        Check ($r.code -eq 0 -and (State) -eq $detached -and @(Entries).Count -eq 0) "${tag}: -PlanOnly changes nothing (exit $($r.code))"

        $r = Run @()
        Check ($r.code -eq 0 -and $r.text -match 'ATTACHED' -and (State) -eq $attached) "${tag}: attach (exit $($r.code))"
        $rec = Get-Content -LiteralPath $record -Raw | ConvertFrom-Json
        Check ($rec.face.sha256 -eq $oursFace -and $rec.blaslt_front.amd_sha256 -eq $amdLt -and $rec.therock -eq '10.3.0a20261030') "${tag}: the record names the four hashes and the build"
        $e = @(Entries)
        $listed = $null
        if ($e.Count -eq 1) { $listed = Get-Content -LiteralPath $e[0].FullName -Raw | ConvertFrom-Json }
        Check ($e.Count -eq 1 -and $null -ne $listed -and $listed.venv -eq $venv -and $r.text -match 'recorded for the uninstaller') "${tag}: attach writes one entry for the uninstaller that names the venv"
        $r = Run @()
        Check ($r.code -eq 0 -and $r.text -match 'already attached' -and (State) -eq $attached -and @(Entries).Count -eq 1) "${tag}: attach again is a no-op (exit $($r.code))"
        $r = Run @('-Check')
        Check ($r.code -eq 0 -and $r.text -match 'CHECK attached' -and $r.text -match 'recorded for the uninstaller: ') "${tag}: -Check on an attached venv (exit $($r.code))"

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
        Check ($r.code -eq 0 -and $r.text -match 'DETACHED' -and (State) -eq $detached -and @(Entries).Count -eq 0) "${tag}: -Detach puts AMD's DLLs back and removes the entry (exit $($r.code))"
        $r = Run @('-Detach')
        Check ($r.code -eq 0 -and (State) -eq $detached) "${tag}: -Detach again is a no-op (exit $($r.code))"

        # A reinstall then a detach: the kept copy goes, AMD's stays live.
        $r = Run @()
        Copy-Item $amdFaceFile $liveFace -Force
        $r = Run @('-Detach')
        Check ($r.code -eq 0 -and (State) -eq $detached) "${tag}: -Detach after a reinstall of rocm_sdk_core (exit $($r.code))"

        # Another TheRock build: refused by every mode, nothing changes.
        Write-Bytes $liveLt 'AMD libhipblaslt of a third nightly'
        $before = State
        foreach ($mode in @(@(), @('-Detach'))) {
            $r = Run $mode
            Check ($r.code -eq 3 -and $r.text -match 'TheRock|another TheRock build' -and (State) -eq $before) "${tag}: $(if ($mode) { $mode[0] } else { 'attach' }) with another AMD build is refused and changes nothing (exit $($r.code))"
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

        # An upgrade of the release: the venv holds our DLLs of the payload before, payload-b is the payload now.
        $r = Run @('-Check') $payloadB
        Check ($r.code -eq 3 -and $r.text -match 'earlier payload') "${tag}: -Check with the next payload says an earlier payload is attached (exit $($r.code))"
        $r = Run @() $payloadB
        Check ($r.code -eq 0 -and (State) -eq $attachedB -and @(Entries).Count -eq 1) "${tag}: attach with the next payload puts in its DLLs (exit $($r.code))"
        $r = Run @('-Detach') $payload
        Check ($r.code -eq 0 -and (State) -eq $detached -and @(Entries).Count -eq 0) "${tag}: -Detach with the payload before still detaches, by the record (exit $($r.code))"

        # The next payload is for another TheRock build: attach is refused, -Detach puts back the AMD DLL that is kept.
        $r = Run @()
        $before = State
        $r = Run @() $payloadC
        Check ($r.code -eq 3 -and $r.text -match 'another TheRock build' -and (State) -eq $before) "${tag}: attach of a payload for another build over an attached venv is refused (exit $($r.code))"
        $r = Run @('-Detach') $payloadC
        Check ($r.code -eq 0 -and (State) -eq $detached) "${tag}: -Detach with that payload puts back the AMD DLLs the record names (exit $($r.code))"

        # Schema 1: the payload file must have the hash that payload.json names.
        Write-Bytes (Join-Path $payload1 'front.dll') 'a front of another build'
        $r = Run @() $payload1
        Check ($r.code -eq 3 -and $r.text -match 'is not the file payload.json names' -and (State) -eq $detached) "${tag}: schema 1, a changed payload file is refused (exit $($r.code))"
        Write-Bytes (Join-Path $payload1 'front.dll') 'our front'
        $r = Run @() $payload1
        Check ($r.code -eq 0 -and (State) -eq $attached) "${tag}: schema 1, attach (exit $($r.code))"
        $r = Run @('-Detach') $payload1

        # A payload file with AMD's bytes is not ours.
        Copy-Item $amdLtFile (Join-Path $payloadB 'front.dll') -Force
        $r = Run @() $payloadB
        Check ($r.code -eq 3 -and $r.text -match 'bytes of AMD' -and (State) -eq $detached) "${tag}: a payload file with AMD's bytes is refused (exit $($r.code))"
        Write-Bytes (Join-Path $payloadB 'front.dll') 'our front of the next release'

        # The list cannot be written (its folder would be under a file): attach works and says so.
        $notADir = Join-Path $Work "file-$tag"
        Write-Bytes $notADir 'a file'
        $r = Run @() $payload $venv (Join-Path $notADir 'list')
        Check ($r.code -eq 0 -and (State) -eq $attached -and $r.text -match 'not recorded for the uninstaller') "${tag}: attach without a writable list warns and still attaches (exit $($r.code))"
        $r = Run @('-Detach') $payload $venv (Join-Path $notADir 'list')
        Check ($r.code -eq 0 -and (State) -eq $detached) "${tag}: -Detach without a writable list (exit $($r.code))"

        # A junction on the path: refused before any change, for the venv itself and for a directory inside it.
        $realCore = Join-Path $Work "real-core-$tag"
        New-Item -ItemType Directory -Force (Join-Path $realCore 'bin') | Out-Null
        Copy-Item $amdFaceFile (Join-Path $realCore 'bin\amdhip64_7.dll')
        $venvJ = Join-Path $Work "venv-junction-$tag"
        New-Item -ItemType Directory -Force (Join-Path $venvJ 'Lib\site-packages\_rocm_sdk_libraries\bin') | Out-Null
        Copy-Item $amdLtFile (Join-Path $venvJ 'Lib\site-packages\_rocm_sdk_libraries\bin\libhipblaslt.dll')
        $link = Join-Path $venvJ 'Lib\site-packages\_rocm_sdk_core'
        New-Item -ItemType Junction -Path $link -Target $realCore | Out-Null
        $junctions += $link
        foreach ($mode in @(@(), @('-Detach'))) {
            $r = Run $mode $payload $venvJ
            Check ($r.code -eq 3 -and $r.text -match 'junction or a link' -and (Sha (Join-Path $realCore 'bin\amdhip64_7.dll')) -eq $script:amdFace -and
                -not (Test-Path -LiteralPath (Join-Path $realCore 'bin\amdhip64_7.dll.orig'))) "${tag}: $(if ($mode) { $mode[0] } else { 'attach' }) with a junction inside the venv is refused and changes nothing (exit $($r.code))"
        }
        $venvLink = Join-Path $Work "venv-link-$tag"
        New-Item -ItemType Junction -Path $venvLink -Target $venv | Out-Null
        $junctions += $venvLink
        $r = Run @() $payload $venvLink
        Check ($r.code -eq 3 -and $r.text -match 'junction or a link' -and (State) -eq $detached) "${tag}: attach through a venv that is a junction is refused (exit $($r.code))"

        # A network path is refused before the first file access.
        $r = Run @('-Detach') $payload '\\bc250-no-such-host.invalid\share\venv'
        Check ($r.code -eq 3 -and $r.text -match 'local drive letter') "${tag}: a network path is refused (exit $($r.code))"

        # Not a TheRock venv.
        $r = Run @() $payload (Join-Path $Work 'nothing')
        Check ($r.code -eq 2) "${tag}: a directory without TheRock is exit 2 (exit $($r.code))"
    }
} finally {
    # A junction goes first, as a link: a recursive removal must not walk into its target.
    foreach ($j in $junctions) { if (Test-Path -LiteralPath $j) { [IO.Directory]::Delete($j) } }
    Remove-Item -LiteralPath $Work -Recurse -Force
}
Write-Host "test-attach-rocm-venv: $script:passes PASS, $script:fails FAIL"
if ($script:fails) { exit 1 }
exit 0
