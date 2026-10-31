# Pack a fixture build with pack-llama-asset.ps1 and read the zip back. The fixture has its own upstream repository,
# patch directory, source copy and build directory, with small PE images in place of the binaries. No compiler, GPU
# or network is used, and no produced binary runs.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$Packer = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
if ([string]::IsNullOrWhiteSpace($Packer)) { $Packer = Join-Path $PSScriptRoot 'pack-llama-asset.ps1' }
$Out = Join-Path ([IO.Path]::GetFullPath($Out)) ('packer-test-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Out)
$utf8 = New-Object Text.UTF8Encoding $false
$results = @()
function Write-Text([string]$path, [string]$text) {
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $path))
    [IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n"), $utf8)
}
function Get-Hash([string]$path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
function Get-BytesHash([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('X2') }) -join '') } finally { $sha.Dispose() }
}
# git for the fixture repositories: no line ending conversion, no signing, no hooks, one identity and one time.
$noHooks = Join-Path $Out 'no-hooks'
[void][IO.Directory]::CreateDirectory($noHooks)
function Invoke-FixtureGit([string]$dir, [string[]]$arguments) {
    $env:GIT_AUTHOR_DATE = '2026-10-31T00:00:00Z'
    $env:GIT_COMMITTER_DATE = '2026-10-31T00:00:00Z'
    try {
        $o = @(& git -C $dir -c core.autocrlf=false -c core.safecrlf=false -c commit.gpgsign=false -c "core.hooksPath=$noHooks" `
            -c user.name=D-Ogi -c user.email=6542335+D-Ogi@users.noreply.github.com @arguments)
        if ($LASTEXITCODE) { throw "fixture git $($arguments -join ' ') failed: $LASTEXITCODE" }
        return $o
    } finally { Remove-Item env:GIT_AUTHOR_DATE, env:GIT_COMMITTER_DATE -ErrorAction SilentlyContinue }
}
# A PE32+ image whose import directory names $imports. The packer only reads it, so it holds nothing else.
function Write-FixturePe([string]$path, [string[]]$imports) {
    $raw = 0x200; $va = 0x1000
    $descSize = 20 * ($imports.Count + 1)
    $names = New-Object IO.MemoryStream
    $nameAt = @()
    foreach ($i in $imports) {
        $nameAt += $descSize + $names.Length
        $bytes = [Text.Encoding]::ASCII.GetBytes($i + [char]0)
        $names.Write($bytes, 0, $bytes.Length)
    }
    $size = $descSize + [int]$names.Length
    $rawSize = [int]([Math]::Ceiling($size / 512) * 512)
    $b = New-Object byte[] ($raw + $rawSize)
    function Put16([int]$o, [int]$v) { [BitConverter]::GetBytes([uint16]$v).CopyTo($b, $o) }
    function Put32([int]$o, [long]$v) { [BitConverter]::GetBytes([uint32]$v).CopyTo($b, $o) }
    Put16 0 0x5A4D; Put32 0x3C 0x40; Put32 0x40 0x4550
    Put16 0x44 0x8664; Put16 0x46 1; Put16 0x54 240; Put16 0x56 0x2022
    $opt = 0x58
    Put16 $opt 0x20B
    Put32 ($opt + 32) 0x1000; Put32 ($opt + 36) 0x200; Put32 ($opt + 56) ($va + 0x1000); Put32 ($opt + 60) $raw
    Put32 ($opt + 108) 16; Put32 ($opt + 120) $va; Put32 ($opt + 124) $descSize
    $sec = $opt + 240
    [Text.Encoding]::ASCII.GetBytes('.idata').CopyTo($b, $sec)
    Put32 ($sec + 8) $size; Put32 ($sec + 12) $va; Put32 ($sec + 16) $rawSize; Put32 ($sec + 20) $raw
    for ($k = 0; $k -lt $imports.Count; $k++) {
        Put32 ($raw + 20 * $k + 12) ($va + $nameAt[$k])
        Put32 ($raw + 20 * $k + 16) $va
    }
    $names.ToArray().CopyTo($b, $raw + $descSize)
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $path))
    [IO.File]::WriteAllBytes($path, $b)
}

# ---- the fixture -----------------------------------------------------------------------------------------------------
$upstream = Join-Path $Out 'upstream'
$baseFiles = [ordered]@{
    '.gitattributes' = "* -text`n"
    'LICENSE' = "MIT License`n`nCopyright (c) the fixture authors`n"
    'licenses/LICENSE-jsonhpp' = "json fixture licence`n"
    'vendor/cpp-httplib/LICENSE' = "httplib fixture licence`n"
    'vendor/hash/xxhash/LICENSE' = "xxhash fixture licence`n"
    'vendor/hash/rotate-bits/LICENSE.md' = "rotate-bits fixture licence`n"
    'vendor/hash/sha256/LICENSE' = "sha256 fixture licence`n"
    'vendor/miniaudio/miniaudio.h' = "int ma;`n/*`nThis software is available as a choice of the following licenses. Choose`nwhichever you prefer.`n*/`nint mb;`n"
    'vendor/stb/stb_image.h' = "int stb;`n/*`n------`nThis software is available under 2 licenses -- choose whichever you prefer.`n------`n*/`n"
    'vendor/sheredom/subprocess.h' = "/*`n   The latest version`n*/`n`n/*`n   This is free and unencumbered software released into the public domain.`n`n     An indented line.`n*/`n"
    'vendor/nlohmann/json.hpp' = "// json`n"
    'src/a.c' = "int a;`n"
    'src/b.c' = "int b;`n"
    'src/c.c' = "int c;`n"
    'src/gone.c' = "int gone;`n"
}
foreach ($k in $baseFiles.Keys) { Write-Text (Join-Path $upstream $k) $baseFiles[$k] }
Invoke-FixtureGit $upstream @('init', '-q') | Out-Null
Invoke-FixtureGit $upstream @('add', '-A') | Out-Null
Invoke-FixtureGit $upstream @('commit', '-q', '-m', 'fixture base') | Out-Null
$base = [string]@(Invoke-FixtureGit $upstream @('rev-parse', 'HEAD'))[0]
$short = $base.Substring(0, 8)

$patchDir = Join-Path $Out 'patches'
Write-Text (Join-Path $patchDir 'fx-one.patch') @"
From: Fixture Author
Subject: [PATCH] the first fixture change

---
diff --git a/src/a.c b/src/a.c
--- a/src/a.c
+++ b/src/a.c
@@ -1 +1,2 @@
 int a;
+int marker_one;
diff --git a/src/new.c b/src/new.c
new file mode 100644
--- /dev/null
+++ b/src/new.c
@@ -0,0 +1 @@
+int marker_new;

"@
Write-Text (Join-Path $patchDir 'fx-one.md') "PROVENANCE: fixture patch by Fixture Author, MIT, from https://example.com/fixture.`n"
Write-Text (Join-Path $patchDir 'fx-two.patch') @"
diff --git a/src/b.c b/src/b.c
--- a/src/b.c
+++ b/src/b.c
@@ -1 +1,2 @@
 int b;
+int marker_two;
diff --git a/src/gone.c b/src/gone.c
deleted file mode 100644
--- a/src/gone.c
+++ /dev/null
@@ -1 +0,0 @@
-int gone;

"@
Write-Text (Join-Path $patchDir 'llama-patches.json') @"
{
  "schema": "bc250.llama-hip-patches/1",
  "base": "$base",
  "patches": [
    { "patch": "fx-one.patch", "file": "src/a.c", "marker": "marker_one", "what": "the first fixture change" },
    { "patch": "fx-two.patch", "file": "src/b.c", "marker": "marker_two", "what": "the second fixture change" }
  ]
}
"@
Write-Text (Join-Path $patchDir 'build-llama-patched.ps1') "# the fixture builder`n"
# The lowest Visual C++ runtime that the installer accepts, read from the commit at the top of the repository.
Write-Text (Join-Path $patchDir 'tools\release\installer\common.ps1') "`$script:VcRuntimeVersion = '14.44.35211.0'`n`$script:VcRuntimeMinMajor = 14`n`$script:VcRuntimeMinMinor = 44`n"
# The packer runs git status here with the configuration of the user, and no line ending setting may make a
# committed fixture file look changed.
Write-Text (Join-Path $patchDir '.gitattributes') "* -text`n"
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'llama-asset-README.txt') -Destination (Join-Path $patchDir 'llama-asset-README.txt')
Invoke-FixtureGit $patchDir @('init', '-q') | Out-Null
Invoke-FixtureGit $patchDir @('add', '-A') | Out-Null
Invoke-FixtureGit $patchDir @('commit', '-q', '-m', 'fixture patches') | Out-Null
$patchCommit = [string]@(Invoke-FixtureGit $patchDir @('rev-parse', 'HEAD'))[0]
$builderHash = Get-Hash (Join-Path $patchDir 'build-llama-patched.ps1')

# A source copy: the base with both patches in. $extra changes it further for a refusal case.
function New-Source([string]$name, [hashtable]$extra) {
    $dir = Join-Path $Out $name
    $files = [ordered]@{}
    foreach ($k in $baseFiles.Keys) { $files[$k] = $baseFiles[$k] }
    $files['src/a.c'] = "int a;`nint marker_one;`n"
    $files['src/new.c'] = "int marker_new;`n"
    $files['src/b.c'] = "int b;`nint marker_two;`n"
    $files.Remove('src/gone.c')
    if ($extra) { foreach ($k in $extra.Keys) { if ($null -eq $extra[$k]) { $files.Remove($k) } else { $files[$k] = $extra[$k] } } }
    foreach ($k in $files.Keys) { Write-Text (Join-Path $dir $k) $files[$k] }
    return $dir
}
$programs = 'llama-server', 'llama-cli', 'llama-completion', 'llama-bench'
function Get-Imports {
    $i = [ordered]@{
        'ggml-base.dll' = @('KERNEL32.dll', 'MSVCP140.dll', 'VCRUNTIME140.dll', 'api-ms-win-crt-runtime-l1-1-0.dll')
        'ggml-cpu.dll' = @('ggml-base.dll', 'KERNEL32.dll')
        'ggml-hip.dll' = @('ggml-base.dll', 'amdhip64.dll', 'bc250hipblas.dll', 'KERNEL32.dll')
        'ggml.dll' = @('ggml-cpu.dll', 'ggml-hip.dll', 'ggml-base.dll', 'KERNEL32.dll')
        'llama.dll' = @('ggml.dll', 'ggml-base.dll', 'KERNEL32.dll')
        'llama-common.dll' = @('llama.dll', 'ggml.dll', 'KERNEL32.dll', 'WS2_32.dll', 'VCRUNTIME140_1.dll')
        'mtmd.dll' = @('llama.dll', 'ggml.dll', 'KERNEL32.dll')
        'llama-perplexity.exe' = @('KERNEL32.dll')
    }
    foreach ($p in $programs) {
        $i["$p.exe"] = @("$p-impl.dll", 'KERNEL32.dll')
        $i["$p-impl.dll"] = @('llama-common.dll', 'llama.dll', 'KERNEL32.dll')
    }
    $i['llama-server-impl.dll'] += 'mtmd.dll'
    $i['llama-cli-impl.dll'] += 'llama-server-impl.dll'
    return $i
}
$rocm = 'X:/fixture/rocm-root'
# A build directory: the cache, the compiler file of CMake, the compile rule of ggml-cuda.cu and the images. $change
# edits the import table, and $msvc is the MSVC version that the C++ compiler simulates.
function New-Build([string]$name, [string]$source, [scriptblock]$change, [string]$msvc = '19.44') {
    $dir = Join-Path $Out $name
    $imports = Get-Imports
    if ($change) { & $change $imports }
    foreach ($k in $imports.Keys) { Write-FixturePe (Join-Path $dir "bin\$k") $imports[$k] }
    $src = $source.Replace('\', '/')
    Write-Text (Join-Path $dir 'CMakeFiles\3.25.2\CMakeCXXCompiler.cmake') "set(CMAKE_CXX_COMPILER_ID `"Clang`")`nset(CMAKE_CXX_SIMULATE_ID `"MSVC`")`nset(CMAKE_CXX_SIMULATE_VERSION `"$msvc`")`n"
    Write-Text (Join-Path $dir 'CMakeCache.txt') @"
CMAKE_CACHE_MAJOR_VERSION:INTERNAL=3
CMAKE_CACHE_MINOR_VERSION:INTERNAL=25
CMAKE_CACHE_PATCH_VERSION:INTERNAL=2
CMAKE_BUILD_TYPE:STRING=Release
BUILD_SHARED_LIBS:BOOL=ON
GGML_HIP:BOOL=ON
GGML_CUDA_FA:BOOL=ON
GGML_CUDA_FORCE_MMQ:BOOL=ON
GGML_LTO:BOOL=ON
LLAMA_BUILD_SERVER:BOOL=ON
CMAKE_CXX_FLAGS:STRING= -DBC250_HIP_NO_DEVICE_PRINTF=1 -DBC250_HIP_MAX_ALLOC_BYTES=0xfffc0000ULL
CMAKE_PREFIX_PATH:UNINITIALIZED=$rocm
CMAKE_HOME_DIRECTORY:INTERNAL=$src
"@
    Write-Text (Join-Path $dir 'build.ninja') @"
build ggml/src/ggml-hip/CMakeFiles/ggml-hip.dir/__/ggml-cuda/ggml-cuda.cu.obj: CXX_COMPILER__ggml-hip_Release $src/ggml/src/ggml-cuda/ggml-cuda.cu
  DEFINES = -DGGML_BACKEND_BUILD
  FLAGS = -x c++  -DBC250_HIP_MAX_ALLOC_BYTES=0xfffc0000ULL -O3 -x hip --offload-arch=gfx1013 --rocm-path=$rocm -std=gnu++17
"@
    return $dir
}
$defaultOptions = '-Jobs 4 -Bc250AllocationCap -Bc250AllocationCapBytes 4294705152 -AssetTools -ApplyPatches'
# A build log with the builder lines, the compiler lines and the hash line of ggml-hip.dll.
function New-Log([string]$name, [string]$build, [string]$builder = $builderHash, [string]$options = $defaultOptions, [switch]$NoHipHash) {
    $path = Join-Path $Out "$name.log"
    $hip = Get-Hash (Join-Path $build 'bin\ggml-hip.dll')
    $lines = @("builder $builder $patchDir\build-llama-patched.ps1", "builder options: $options",
        'cmake configure: -S fixture -B fixture -G Ninja -DGGML_HIP:BOOL=ON',
        '-- The C compiler identification is Clang 22.1.8 with GNU-like command-line',
        '-- The CXX compiler identification is Clang 22.1.8 with GNU-like command-line',
        '[1/1] Linking CXX shared library bin\ggml-hip.dll')
    if (-not $NoHipHash) { $lines += "SHA256          $hip       $build\bin\ggml-hip.dll" }
    Write-Text $path (($lines -join "`n") + "`n")
    return $path
}
$source = New-Source 'source' $null
$build = New-Build 'build' $source $null
$log = New-Log 'build' $build
$measured = Join-Path $Out 'measured.txt'
Write-Text $measured "MEASURED ON A BC-250 (fixture)`n`nThe fixture measures nothing.`n"
$release = '0.7.216.999-tester.99'
$name = "llama.cpp-hip-gfx1013-$short-$release"

function Invoke-Pack([string]$case, [hashtable]$change) {
    $outDir = Join-Path $Out "out-$case"
    $a = @{ Build = $build; BuildLog = $log; UpstreamRepo = $upstream; Release = $release; MeasuredFile = $measured
        OutDir = $outDir; PatchDir = $patchDir }
    if ($change) { foreach ($k in $change.Keys) { $a[$k] = $change[$k] } }
    $r = @{ refused = $false; message = ''; printed = @(); out = $a.OutDir }
    try { $r.printed = @(& $Packer @a) } catch { $r.refused = $true; $r.message = $_.Exception.Message }
    return $r
}
# A refused case writes nothing: no zip, no partial zip and no work directory. $Keep names a file that was there before.
function Assert-Refused([string]$case, [hashtable]$change, [string]$pattern, [string]$Keep = '') {
    $r = Invoke-Pack $case $change
    if (-not $r.refused) { throw "case $case was packed and must be refused" }
    if ($r.message -notmatch $pattern) { throw "case $case was refused for another reason: $($r.message)" }
    if (Test-Path -LiteralPath $r.out) {
        $left = @(Get-ChildItem -LiteralPath $r.out -Force | Where-Object { $_.Name -ne $Keep })
        if ($left.Count) { throw "case $case left files in its output directory: $(($left | ForEach-Object Name) -join ', ')" }
    }
    $script:results += @{ name = $case; passed = $true; refused = $true; message = $r.message }
}
function Read-Zip([string]$path) {
    $zip = [IO.Compression.ZipFile]::OpenRead($path)
    try {
        $entries = [ordered]@{}
        foreach ($e in $zip.Entries) {
            $s = $e.Open(); $m = New-Object IO.MemoryStream
            try { $s.CopyTo($m) } finally { $s.Dispose() }
            $entries[$e.FullName] = @{ bytes = $m.ToArray(); time = $e.LastWriteTime.DateTime }
        }
        return $entries
    } finally { $zip.Dispose() }
}

# ---- the asset of the fixture ----------------------------------------------------------------------------------------
$r = Invoke-Pack 'pack' $null
if ($r.refused) { throw "the fixture was refused: $($r.message)" }
$zipPath = Join-Path $r.out "$name.zip"
if (-not (Test-Path -LiteralPath $zipPath)) { throw "no $zipPath" }
$zip = Read-Zip $zipPath
$expected = @('build.log', 'LICENSE', 'licenses/LICENSE-jsonhpp', 'licenses/LICENSE-httplib', 'licenses/LICENSE-xxhash',
    'licenses/LICENSE-rotate-bits', 'licenses/LICENSE-sha256', 'licenses/LICENSE-miniaudio', 'licenses/LICENSE-stb_image',
    'licenses/LICENSE-subprocess', 'patches/fx-one.patch', 'patches/fx-two.patch', 'patches/llama-patches.json',
    "patches/source-vs-upstream-$short.diff", 'PROVENANCE.txt', 'README.txt', 'manifest.json',
    'ggml.dll', 'ggml-base.dll', 'ggml-cpu.dll', 'ggml-hip.dll', 'llama.dll', 'llama-common.dll', 'mtmd.dll') +
    @($programs | ForEach-Object { "$_.exe"; "$_-impl.dll" })
$got = @($zip.Keys | ForEach-Object { if (-not $_.StartsWith("$name/")) { throw "the entry $_ is outside the folder $name" }; $_.Substring($name.Length + 1) })
$missing = @($expected | Where-Object { $got -notcontains $_ })
$extra = @($got | Where-Object { $expected -notcontains $_ })
if ($missing.Count -or $extra.Count) { throw "the zip entries differ: missing $($missing -join ', '), extra $($extra -join ', ')" }
$patchEntries = @($got | Where-Object { $_ -like 'patches/*' })
if (($patchEntries -join ',') -ne "patches/fx-one.patch,patches/fx-two.patch,patches/llama-patches.json,patches/source-vs-upstream-$short.diff") {
    throw "the patches are not in the order they apply: $($patchEntries -join ', ')"
}
function Get-EntryText([string]$path) { return $utf8.GetString($zip["$name/$path"].bytes) }
foreach ($k in $zip.Keys) { if ($zip[$k].time -ne [datetime]'2026-10-31T00:00:00') { throw "$k carries the time $($zip[$k].time), not the commit time" } }
$manifest = Get-EntryText 'manifest.json' | ConvertFrom-Json
if ($manifest.name -ne $name -or $manifest.'llama.cpp' -ne $base -or $manifest.'amdgpu-wddm' -ne $patchCommit -or $manifest.release -ne $release) { throw 'manifest.json names the wrong asset, base, commit or release' }
if ((@($manifest.runtime_not_included) -join ',') -ne 'amdhip64.dll,bc250hipblas.dll') { throw "runtime_not_included is $(@($manifest.runtime_not_included) -join ',')" }
$listed = @($manifest.files.PSObject.Properties | ForEach-Object Name)
if ($listed.Count -ne $expected.Count - 1 -or $listed -contains 'manifest.json') { throw 'manifest.json does not list every other file once' }
foreach ($f in $listed) { if ($manifest.files.$f -ne (Get-BytesHash $zip["$name/$f"].bytes)) { throw "manifest.json has another SHA-256 for $f" } }
if ($manifest.files.'ggml-hip.dll' -ne (Get-Hash (Join-Path $build 'bin\ggml-hip.dll'))) { throw 'the zip holds another ggml-hip.dll' }
$readme = Get-EntryText 'README.txt'
foreach ($want in "amdgpu-wddm $release installed", "runtime files from there:`r`n  amdhip64.dll and bc250hipblas.dll.`r`n", '0xFFFC0000 bytes (4 GiB minus 256 KiB)',
    'Redistributable (x64), version 14.44 or newer.',
    "MEASURED ON A BC-250 (fixture)`r`n`r`nThe fixture measures nothing.`r`n", "$name`r`n") {
    if (-not $readme.Contains($want)) { throw "README.txt does not hold: $want" }
}
if ($readme -match '\{\{' -or $readme -match "[^`r]`n") { throw 'README.txt holds a placeholder or a line without CRLF' }
$provenance = Get-EntryText 'PROVENANCE.txt'
foreach ($want in "upstream revision $base.", ' 1. patches/fx-one.patch: the first fixture change (src/a.c). By Fixture Author, from https://example.com/fixture.',
    " 2. patches/fx-two.patch: the second fixture change (src/b.c). By D-Ogi.`r`n", "commit $patchCommit", "build-llama-patched.ps1 $defaultOptions",
    "(SHA-256 $builderHash)", "ggml-hip.dll SHA-256 $($manifest.files.'ggml-hip.dll').", '--rocm-path=<rocm-root>',
    'Compiler: Clang 22.1.8 with GNU-like command-line for C and C++.', 'the programs need the Visual C++ runtime 14.44 or newer.') {
    if (-not $provenance.Contains($want)) { throw "PROVENANCE.txt does not hold: $want" }
}
if ((Get-EntryText 'licenses/LICENSE-miniaudio') -ne "This software is available as a choice of the following licenses. Choose`r`nwhichever you prefer.`r`n") { throw 'LICENSE-miniaudio is not the comment block of miniaudio.h' }
if ((Get-EntryText 'licenses/LICENSE-subprocess') -ne "This is free and unencumbered software released into the public domain.`r`n`r`n  An indented line.`r`n") { throw 'LICENSE-subprocess is not the comment block of subprocess.h without its indentation' }
if ((Get-EntryText 'licenses/LICENSE-xxhash') -ne $baseFiles['vendor/hash/xxhash/LICENSE']) { throw 'LICENSE-xxhash is not a copy of vendor/hash/xxhash/LICENSE' }
$results += @{ name = 'pack'; passed = $true; refused = $false; message = "$($got.Count) entries" }

# The difference in the zip turns the base files into the source copy.
$check = Join-Path $Out 'apply-check'
[void][IO.Directory]::CreateDirectory($check)
$tar = Join-Path $Out 'base.tar'
& git -C $upstream -c core.autocrlf=false archive --format=tar -o $tar $base -- src
if ($LASTEXITCODE) { throw 'git archive of the fixture base failed' }
& (Join-Path ([Environment]::SystemDirectory) 'tar.exe') -xf $tar -C $check
if ($LASTEXITCODE) { throw 'tar of the fixture base failed' }
$diffFile = Join-Path $Out 'zip.diff'
[IO.File]::WriteAllBytes($diffFile, $zip["$name/patches/source-vs-upstream-$short.diff"].bytes)
# Outside every repository, as GNU patch would apply it.
$env:GIT_CEILING_DIRECTORIES = $Out
try { & git -C $check -c core.autocrlf=false apply -p1 --whitespace=nowarn $diffFile } finally { Remove-Item env:GIT_CEILING_DIRECTORIES }
if ($LASTEXITCODE) { throw 'the difference of the zip does not apply on the base' }
foreach ($f in 'src/a.c', 'src/b.c', 'src/c.c', 'src/new.c') {
    if ((Get-Hash (Join-Path $check $f)) -ne (Get-Hash (Join-Path $source $f))) { throw "after the difference, $f is not the file of the source copy" }
}
if (Test-Path -LiteralPath (Join-Path $check 'src/gone.c')) { throw 'the difference does not delete src/gone.c' }
$results += @{ name = 'difference'; passed = $true; refused = $false; message = 'the difference of the zip gives the source copy from the base' }

# The same inputs give the same zip. A zip of the same name is replaced only with -Force.
$zipHash = Get-Hash $zipPath
$again = Invoke-Pack 'again' $null
if ($again.refused) { throw "the second pack was refused: $($again.message)" }
if ((Get-Hash (Join-Path $again.out "$name.zip")) -ne $zipHash) { throw 'the same inputs gave another zip' }
$results += @{ name = 'same-bytes'; passed = $true; refused = $false; message = $zipHash }
Assert-Refused 'exists' @{ OutDir = $r.out } 'exists\. Pass -Force' -Keep "$name.zip"
if ((Get-Hash $zipPath) -ne $zipHash) { throw 'a refused pack changed the zip that was there' }
$forced = Invoke-Pack 'force' @{ OutDir = $r.out; Force = $true }
if ($forced.refused -or (Get-Hash $zipPath) -ne $zipHash) { throw "-Force did not give the same zip again: $($forced.message)" }
$results += @{ name = 'force'; passed = $true; refused = $false; message = 'the same zip again over the old one' }

# ---- refusals --------------------------------------------------------------------------------------------------------
$b = New-Build 'build-runtime' $source $null
Copy-Item -LiteralPath (Join-Path $b 'bin\ggml.dll') -Destination (Join-Path $b 'bin\amdhip64.dll')
Assert-Refused 'runtime-in-build' @{ Build = $b; BuildLog = (New-Log 'runtime' $b) } 'holds HIP runtime files \(amdhip64\.dll\)'
$b = New-Build 'build-missing' $source $null
Remove-Item -LiteralPath (Join-Path $b 'bin\llama-bench.exe')
Assert-Refused 'missing-binary' @{ Build = $b; BuildLog = (New-Log 'missing' $b) } 'has no bin\\llama-bench\.exe'
$b = New-Build 'build-import' $source { param($i) $i['llama-common.dll'] += 'fixture-missing.dll' }
Assert-Refused 'unknown-import' @{ Build = $b; BuildLog = (New-Log 'import' $b) } 'llama-common\.dll imports fixture-missing\.dll'
$b = New-Build 'build-hipblas' $source { param($i) $i['ggml-hip.dll'] += 'hipblas.dll' }
Assert-Refused 'foreign-runtime-import' @{ Build = $b; BuildLog = (New-Log 'hipblas' $b) } 'ggml-hip\.dll imports hipblas\.dll'
$b = New-Build 'build-msvc' $source $null '19.50'
Assert-Refused 'vc-runtime-newer' @{ Build = $b; BuildLog = (New-Log 'msvc' $b) } 'needs the Visual C\+\+ runtime 14\.50 or newer, and the installer of the commit accepts 14\.44'
Assert-Refused 'builder-mismatch' @{ BuildLog = (New-Log 'builder' $build ('0' * 64)) } 'names the builder 0{64}'
Assert-Refused 'not-this-log' @{ BuildLog = (New-Log 'nohash' $build -NoHipHash) } 'is not the log of this build'
Assert-Refused 'no-asset-tools' @{ BuildLog = (New-Log 'noasset' $build $builderHash '-Jobs 4 -Bc250AllocationCap -Bc250AllocationCapBytes 4294705152') } 'without -AssetTools'
Assert-Refused 'no-cap' @{ BuildLog = (New-Log 'nocap' $build $builderHash '-Jobs 4 -AssetTools') } 'without -Bc250AllocationCap'
Assert-Refused 'cap-disagrees' @{ BuildLog = (New-Log 'capother' $build $builderHash '-Jobs 4 -Bc250AllocationCap -Bc250AllocationCapBytes 1073741824 -AssetTools') } 'does not hold -DBC250_HIP_MAX_ALLOC_BYTES=0x40000000ULL'
$s = New-Source 'source-marker' @{ 'src/b.c' = "int b;`nint marker_tw0;`n" }
$b = New-Build 'build-marker' $s $null
Assert-Refused 'missing-marker' @{ Build = $b; BuildLog = (New-Log 'marker' $b) } 'lacks the marker of fx-two\.patch'
$s = New-Source 'source-unlisted' @{ 'src/c.c' = "int c2;`n" }
$b = New-Build 'build-unlisted' $s $null
Assert-Refused 'unlisted-change' @{ Build = $b; BuildLog = (New-Log 'unlisted' $b) } 'differs from \w{8} in 1 file\(s\) that no patch of llama-patches\.json changes: src/c\.c'
$s = New-Source 'source-kept' @{ 'src/gone.c' = $baseFiles['src/gone.c'] }
$b = New-Build 'build-kept' $s $null
Assert-Refused 'base-file-kept' @{ Build = $b; BuildLog = (New-Log 'kept' $b) } 'the patches change src/gone\.c, and the source copy holds the base version'
Assert-Refused 'source-mismatch' @{ Source = (Join-Path $Out 'source-kept') } 'the build compiled'
$bad = Join-Path $Out 'measured-bad.txt'
Write-Text $bad "Some numbers without a heading`n"
Assert-Refused 'measured-heading' @{ MeasuredFile = $bad } 'is not a MEASURED heading'
Assert-Refused 'release-name' @{ Release = 'tester 35' } 'holds a character that the asset name cannot hold'
Add-Content -LiteralPath (Join-Path $patchDir 'llama-asset-README.txt') -Value 'an edit that is not committed'
try { Assert-Refused 'dirty-patch-dir' $null 'changes that are not committed' }
finally { Invoke-FixtureGit $patchDir @('checkout', '-q', '--', 'llama-asset-README.txt') | Out-Null }

@{ packer_sha256 = (Get-Hash $Packer); scope = 'pack-llama-asset.ps1 on a fixture build; no compiler, GPU or network'; cases = $results } |
    ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out 'results.json')
Write-Output "PASS $($results.Count) packer cases; receipts: $Out"
