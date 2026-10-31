# Invoke the actual builder through a recording CMake fixture. No compiler or GPU runs.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$Builder = '',
    # A reviewed source copy that already carries the complete set, used as the reference of the
    # -ApplyPatches case: the three touched files are reverse-patched out of it and the patches are
    # applied again, so the result must match it byte for byte. The case is skipped without it, so
    # the test still runs where no such copy exists.
    [string]$ApplyFrom = '',
    # A git repository that holds the upstream base commit of llama-patches.json (a clone of
    # llama.cpp). With it, the base's copies of every file the patches touch are taken out of the
    # repository, and the builder must apply the whole list on them, in order, with git apply. The
    # case is skipped without it. The repository is only read.
    [string]$UpstreamRepo = ''
)
$ErrorActionPreference='Stop'
if ([string]::IsNullOrWhiteSpace($Builder)) {
    $Builder = Join-Path $PSScriptRoot 'build-llama-patched.ps1'
}
$Out=Join-Path $Out ('builder-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($Out)
$patchList = Get-Content (Join-Path $PSScriptRoot 'llama-patches.json') -Raw | ConvertFrom-Json
$patchNames = @($patchList.patches | ForEach-Object { [string]$_.patch })
$results=@()
# The list and the directory agree: every patch file here is in the list once, every entry has its file,
# and every port of another project's patch has its .md with a PROVENANCE line. No patch names an AI
# model as an author or co-author (repository rule 10), whatever the source patch carried.
$onDisk = @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.patch' | ForEach-Object Name)
foreach($name in $onDisk){ if($patchNames -notcontains $name){throw "patch file not in llama-patches.json: $name"} }
if(@($patchNames|Select-Object -Unique).Count -ne $patchNames.Count){throw 'a patch is named twice in llama-patches.json'}
foreach($name in $patchNames){
    $path=Join-Path $PSScriptRoot $name
    if(!(Test-Path -LiteralPath $path)){throw "llama-patches.json names a missing patch: $name"}
    if(Select-String -LiteralPath $path -Pattern '^(Co-authored|Assisted|Generated)-by:' -Quiet){throw "$name carries an AI author trailer"}
    if($name -like 'llama-rdna1-*'){
        $md=[IO.Path]::ChangeExtension($path,'.md')
        if(!(Test-Path -LiteralPath $md)){throw "$name has no .md next to it"}
        if(!(Select-String -LiteralPath $md -Pattern '^PROVENANCE: .*MIT' -Quiet)){throw "$([IO.Path]::GetFileName($md)) has no PROVENANCE line naming MIT"}
        if(!(Select-String -LiteralPath $path -Pattern '^From: \S' -Quiet)){throw "$name lost its From: author header"}
    }
}
$results+=@{name='patch-list';passed=$true;refused=$false;message="$($patchNames.Count) patches, list and directory agree"}
# A fixture source that carries every marker of the complete HIP source set. Each marker lives in the
# file the builder reads it from, so a marker that moves to another file fails here.
function New-FixtureSource([string]$root, [string[]]$without) {
    $byFile=[ordered]@{}
    foreach($p in $patchList.patches){
        if(!$byFile.Contains($p.file)){$byFile[$p.file]=@()}
        if($without -notcontains $p.marker){$byFile[$p.file]+=[string]$p.marker}
    }
    foreach($rel in $byFile.Keys){
        $path=Join-Path $root $rel
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $path))
        Set-Content $path $byFile[$rel]
    }
    return $root
}
$source = New-FixtureSource (Join-Path $Out 'source') @()
# The source sets that must be refused, one per missing change of the complete set. Before the set was
# complete in this directory, the one without the gfx1013 entry built without a word and produced a
# binary whose device code disagreed with the host about the launch geometry of the MoE expert kernel.
$incomplete = @($patchList.patches | ForEach-Object {
    @{ name = 'no-' + [IO.Path]::GetFileNameWithoutExtension([string]$_.patch); marker = [string]$_.marker }
})
$shim=Join-Path $Out 'cmake-shim.ps1'
@'
$a=@($args)
if($a[0] -eq '--build') {
    $b=$a[1]
    ConvertTo-Json -InputObject $a | Set-Content (Join-Path $b 'build-args.json')
    [void][IO.Directory]::CreateDirectory((Join-Path $b 'bin'))
    Set-Content (Join-Path $b 'bin/ggml-hip.dll') 'host fixture only; not a binary'
} else {
    $b=$a[[Array]::IndexOf($a,'-B')+1]
    [void][IO.Directory]::CreateDirectory($b)
    ConvertTo-Json -InputObject $a | Set-Content (Join-Path $b 'configure-args.json')
    $cache=@{}
    if(Test-Path (Join-Path $b 'CMakeCache.txt')) {
        foreach($l in Get-Content (Join-Path $b 'CMakeCache.txt')) {
            if($l -match '^([^:=]+)(?::([^=]+))?=(.*)$'){$cache[$Matches[1]]=$l}
        }
    }
    foreach($v in $a){
        if($v -match '^-D([^:=]+)(?::([^=]+))?=(.*)$') {
            $name=$Matches[1];$type=$Matches[2];$value=$Matches[3]
            if(!$type){$type='STRING'}
            $cache[$name]="${name}:${type}=$value"
        } elseif($v -match '^-U(.+)$'){$cache.Remove($Matches[1])}
    }
    $cache.Values|Sort-Object|Set-Content (Join-Path $b 'CMakeCache.txt')
}
$global:LASTEXITCODE=0
'@ | Set-Content $shim
$cases=@(
    @{name='normal';extra='';existing='';cap=$false;validation=$true;asset=$false;nolto=$false;refuse=$false},
    @{name='cap';extra='';existing='';cap=$true;validation=$false;asset=$false;nolto=$false;refuse=$false},
    @{name='asset';extra='';existing='';cap=$true;validation=$false;asset=$true;nolto=$false;refuse=$false},
    @{name='asset-validation-no-lto';extra='';existing='';cap=$true;validation=$true;asset=$true;nolto=$true;refuse=$false},
    @{name='input-no-fa';extra='CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DGGML_CUDA_NO_FA';existing='';cap=$false;validation=$false;asset=$false;nolto=$false;refuse=$true},
    @{name='existing-no-fa';extra='';existing='CMAKE_HIP_FLAGS_RELEASE:STRING=-DGGML_CUDA_NO_FA';cap=$false;validation=$false;asset=$false;nolto=$false;refuse=$true},
    @{name='input-cap';extra='CMAKE_CXX_FLAGS:STRING=-DBC250_HIP_MAX_ALLOC_BYTES=7';existing='';cap=$false;validation=$false;asset=$false;nolto=$false;refuse=$true}
)
foreach($case in $cases){
    $build=Join-Path $Out $case.name
    [void][IO.Directory]::CreateDirectory($build)
    @('GGML_CUDA_FA:BOOL=OFF','GGML_CUDA_FA_QUANTS:STRING=q4_0','GGML_CUDA_FA_ALL_QUANTS:BOOL=OFF',$case.existing)|Set-Content (Join-Path $build 'CMakeCache.txt')
    $cache=Join-Path $build 'original.txt'
    @('GGML_CUDA_FA:BOOL=OFF','GGML_CUDA_FA_QUANTS:STRING=q4_0','GGML_CUDA_FA_ALL_QUANTS:BOOL=OFF','GGML_HIP:BOOL=ON','GGML_NATIVE:BOOL=OFF','LLAMA_BUILD_COMMON:BOOL=OFF','LLAMA_BUILD_TESTS:BOOL=OFF','LLAMA_BUILD_TOOLS:BOOL=OFF','CMAKE_CXX_FLAGS:STRING=-O2 -DKEEP=123','CMAKE_CXX_FLAGS_RELEASE:STRING=-O3','CMAKE_C_FLAGS:STRING=-DKEEP_C=7','CMAKE_CXX_COMPILER:FILEPATH=X:/compiler.exe',$case.extra)|Set-Content $cache
    $old=$env:ROCM_PATH;$env:ROCM_PATH='host-test-prior'
    $refused=$false;$message='';$log=@()
    try {$log=@(& $Builder -Source $source -Build $build -RocmRoot 'fixture-rocm' -OriginalCache $cache -CMake $shim -Bc250AllocationCap:$case.cap -ValidationTools:$case.validation -AssetTools:$case.asset -NoLto:$case.nolto)}
    catch {$refused=$true;$message=$_.Exception.Message}
    if($env:ROCM_PATH -ne 'host-test-prior'){throw "environment restoration: $($case.name)"}
    $env:ROCM_PATH=$old
    if($refused -ne $case.refuse){throw "wrong refusal $($case.name): $message"}
    $built=Test-Path (Join-Path $build 'build-args.json')
    if($case.refuse -and $built){throw 'refused configuration started a build'}
    if(!$case.refuse){
        $final=Get-Content (Join-Path $build 'CMakeCache.txt')
        foreach($required in @('GGML_CUDA_FA:BOOL=ON','GGML_HIP:BOOL=ON','GGML_NATIVE:BOOL=OFF','CMAKE_C_FLAGS:STRING=-DKEEP_C=7','CMAKE_CXX_COMPILER:FILEPATH=X:/compiler.exe')){if($final -notcontains $required){throw "lost option $required"}}
        if($final -match '^GGML_CUDA_FA_(ALL_QUANTS|QUANTS):'){throw 'quant override was not removed'}
        $flags=@($final|Where-Object{$_ -match '^CMAKE_CXX_FLAGS:'})[0]
        if($flags -notmatch '-O2 -DKEEP=123 -DBC250_HIP_NO_DEVICE_PRINTF=1'){throw 'lost unrelated CXX flags'}
        if(($flags -match 'BC250_HIP_MAX_ALLOC_BYTES') -ne $case.cap){throw 'cap opt-in changed'}
        $targets=Get-Content (Join-Path $build 'build-args.json') -Raw|ConvertFrom-Json
        if($targets -notcontains 'ggml-hip'){throw 'missing HIP target'}
        if($case.validation){
            foreach($target in @('test-backend-ops','llama-completion','llama-bench','llama-perplexity')){if($targets -notcontains $target){throw "missing target $target"}}
            foreach($option in @('LLAMA_BUILD_COMMON:BOOL=ON','LLAMA_BUILD_TESTS:BOOL=ON','LLAMA_BUILD_TOOLS:BOOL=ON')){if($final -notcontains $option){throw "missing option $option"}}
        }
        if($case.asset){
            foreach($target in @('llama-server','llama-cli','llama-completion','llama-bench')){if($targets -notcontains $target){throw "missing target $target"}}
            foreach($option in @('LLAMA_BUILD_COMMON:BOOL=ON','LLAMA_BUILD_TOOLS:BOOL=ON','LLAMA_BUILD_SERVER:BOOL=ON','LLAMA_BUILD_UI:BOOL=OFF','LLAMA_USE_PREBUILT_UI:BOOL=OFF')){if($final -notcontains $option){throw "missing option $option"}}
        } elseif(($targets -contains 'llama-server') -or ($targets -contains 'llama-cli')){throw 'asset targets without -AssetTools'}
        $first=[Array]::IndexOf($targets,'--target')+1;$last=[Array]::IndexOf($targets,'-j')-1
        $named=@($targets[$first..$last])
        if(@($named|Select-Object -Unique).Count -ne $named.Count){throw 'a target is named twice'}
        if(($final -contains 'GGML_LTO:BOOL=ON') -eq $case.nolto){throw 'LTO default changed'}
        # The log lines that pack-llama-asset.ps1 reads: the builder with its SHA-256 first, the options
        # line without a path, and the configure line with every CMake option.
        $lines=@($log|ForEach-Object{[string]$_})
        if($lines[0] -notmatch '^builder ([0-9A-F]{64}) ' -or $Matches[1] -ne (Get-FileHash $Builder -Algorithm SHA256).Hash){throw "the first log line does not name the builder and its SHA-256: $($lines[0])"}
        $opt=@($lines|Where-Object{$_ -like 'builder options: *'})
        if($opt.Count -ne 1){throw 'no single builder options line'}
        if($opt[0].Contains($build) -or $opt[0].Contains($source)){throw 'the options line holds a path'}
        foreach($s in @(@{n='-AssetTools';on=$case.asset},@{n='-ValidationTools';on=$case.validation},@{n='-NoLto';on=$case.nolto},@{n='-Bc250AllocationCap';on=$case.cap})){
            if((" $($opt[0]) " -match " $([regex]::Escape($s.n)) ") -ne $s.on){throw "options line and $($s.n) disagree: $($opt[0])"}
        }
        if(!@($lines|Where-Object{$_ -like 'cmake configure: *' -and $_.Contains('-DGGML_CUDA_FA:BOOL=ON')}).Count){throw 'no configure line with the CMake options'}
    }
    $results+=@{name=$case.name;passed=$true;refused=$refused;message=$message}
}
# An incomplete source set is refused before anything is configured, and the refusal names the patch.
foreach($case in $incomplete){
    $build=Join-Path $Out ('incomplete-'+$case.name)
    [void][IO.Directory]::CreateDirectory($build)
    $bad=New-FixtureSource (Join-Path $Out ('source-'+$case.name)) @($case.marker)
    $cache=Join-Path $build 'original.txt'
    @('GGML_HIP:BOOL=ON','CMAKE_CXX_FLAGS:STRING=')|Set-Content $cache
    $refused=$false;$message=''
    try {& $Builder -Source $bad -Build $build -RocmRoot 'fixture-rocm' -OriginalCache $cache -CMake $shim | Out-Null}
    catch {$refused=$true;$message=$_.Exception.Message}
    if(!$refused){throw "an incomplete source set was accepted: $($case.name)"}
    if($message -notmatch "is missing 1 of the $($patchNames.Count) HIP source changes"){throw "unclear refusal for $($case.name): $message"}
    if(Test-Path (Join-Path $build 'configure-args.json')){throw "refused source set reached CMake: $($case.name)"}
    $results+=@{name='incomplete-'+$case.name;passed=$true;refused=$true;message=$message}
}
# The files the patches touch, from their ---/+++ lines (some patches have no "diff --git" line), and
# the ones they create (--- /dev/null).
$touched=@(); $created=@()
foreach($name in $patchNames){
    $lines=@(Get-Content -LiteralPath (Join-Path $PSScriptRoot $name))
    for($i=0;$i+1 -lt $lines.Count;$i++){
        if($lines[$i] -match '^--- (\S+)' -and $lines[$i+1] -match '^\+\+\+ b/(\S+)'){
            $path=$Matches[1]
            $touched+=$path
            if($lines[$i] -match '^--- /dev/null'){$created+=$path}
        }
    }
}
$touched=@($touched|Select-Object -Unique); $created=@($created|Select-Object -Unique)
$existing=@($touched|Where-Object{$created -notcontains $_})
function Invoke-ApplyCase([string]$name, [string]$clean) {
    $build=Join-Path $Out $name
    [void][IO.Directory]::CreateDirectory($build)
    $cache=Join-Path $build 'original.txt'
    @('GGML_HIP:BOOL=ON','CMAKE_CXX_FLAGS:STRING=')|Set-Content $cache
    $applied=& $Builder -Source $clean -Build $build -RocmRoot 'fixture-rocm' -OriginalCache $cache -CMake $shim -ApplyPatches
    if($LASTEXITCODE){throw "the builder failed with -ApplyPatches ($name)"}
    $order=@($applied|Where-Object{$_ -like 'applied: *'}|ForEach-Object{$_.Substring(9)})
    if(($order -join ',') -ne ($patchNames -join ',')){throw "-ApplyPatches ($name) did not apply the list in order: $($applied -join '; ')"}
}
# -ApplyPatches turns an unpatched copy of the real upstream files into a complete one, with the real
# patch files and git apply. The touched files are taken from the reference source copy and the whole
# list is reverse-patched out of them, last patch first, so the test owns no copy of upstream.
if($ApplyFrom){
    $clean=Join-Path $Out 'apply-source'
    foreach($rel in $touched){
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent (Join-Path $clean $rel)))
        Copy-Item (Join-Path $ApplyFrom $rel) (Join-Path $clean $rel) -Force
    }
    & git -C $clean init -q
    if($LASTEXITCODE){throw 'git init failed in the apply fixture'}
    for($i=$patchNames.Count-1;$i -ge 0;$i--){
        & git -C $clean apply -R -p1 -- (Join-Path $PSScriptRoot $patchNames[$i])
        if($LASTEXITCODE){throw "could not reverse $($patchNames[$i]) out of the reference copy"}
    }
    Invoke-ApplyCase 'apply' $clean
    foreach($rel in $touched){
        $a=(Get-Content (Join-Path $clean $rel) -Raw) -replace "`r`n","`n"
        $b=(Get-Content (Join-Path $ApplyFrom $rel) -Raw) -replace "`r`n","`n"
        if($a -ne $b){throw "after -ApplyPatches, $rel does not match the reference copy"}
    }
    $results+=@{name='apply-patches';passed=$true;refused=$false;message="all $($patchNames.Count) applied in order; $($touched.Count) files match the reference copy"}
}
# The whole list applies on the upstream base commit itself, in order, with nothing else in between.
if($UpstreamRepo){
    $base=[string]$patchList.base
    & git -C $UpstreamRepo cat-file -e "$base^{commit}"
    if($LASTEXITCODE){throw "$UpstreamRepo does not hold the base commit $base"}
    foreach($rel in $existing){
        & git -C $UpstreamRepo cat-file -e "${base}:$rel"
        if($LASTEXITCODE){throw "the base commit has no $rel, which a patch changes"}
    }
    foreach($rel in $created){
        # ls-tree prints nothing for a missing path; cat-file -e would write to stderr, which this
        # PowerShell turns into a terminating error
        if(@(& git -C $UpstreamRepo ls-tree --name-only $base -- $rel).Count){throw "the base commit already has $rel, which a patch creates"}
    }
    $clean=Join-Path $Out 'upstream-source'
    [void][IO.Directory]::CreateDirectory($clean)
    $tar=Join-Path $Out 'upstream-base.tar'
    & git -C $UpstreamRepo archive --format=tar -o $tar $base -- @existing
    if($LASTEXITCODE){throw 'git archive of the base failed'}
    & tar -xf $tar -C $clean
    if($LASTEXITCODE){throw 'could not extract the base files'}
    & git -C $clean init -q
    if($LASTEXITCODE){throw 'git init failed in the upstream fixture'}
    Invoke-ApplyCase 'apply-upstream' $clean
    $results+=@{name='apply-upstream';passed=$true;refused=$false;message="all $($patchNames.Count) applied in order on $base"}
}
@{builder_sha256=(Get-FileHash $Builder).Hash;scope='Actual builder with recording CMake shim; no compiler or GPU executed';reference_source=$ApplyFrom;cases=$results}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $Out 'results.json')
Write-Output "PASS $($results.Count) builder cases; receipts: $Out"
