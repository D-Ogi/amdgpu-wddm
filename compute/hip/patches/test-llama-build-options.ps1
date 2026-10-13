# Invoke the actual builder through a recording CMake fixture. No compiler or GPU runs.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$Builder = '',
    # A reviewed source copy that already carries the complete set, used as the reference of the
    # -ApplyPatches case: the three touched files are reverse-patched out of it and the patches are
    # applied again, so the result must match it byte for byte. The case is skipped without it, so
    # the test still runs where no such copy exists.
    [string]$ApplyFrom = ''
)
$ErrorActionPreference='Stop'
if ([string]::IsNullOrWhiteSpace($Builder)) {
    $Builder = Join-Path $PSScriptRoot 'build-llama-patched.ps1'
}
$out=Join-Path $Out ('builder-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($out)
# A fixture source that carries all four markers of the complete HIP source set. Each marker lives in
# the file the builder reads it from, so a marker that moves to another file fails here.
function New-FixtureSource([string]$root, [string[]]$without) {
    [void][IO.Directory]::CreateDirectory((Join-Path $root 'ggml/src/ggml-cuda/vendors'))
    $common = @('#ifndef BC250_HIP_NO_DEVICE_PRINTF', '!defined(BC250_HIP_NO_ASM_DP4A)') |
        Where-Object { $without -notcontains $_ }
    Set-Content (Join-Path $root 'ggml/src/ggml-cuda/common.cuh') $common
    Set-Content (Join-Path $root 'ggml/src/ggml-cuda/ggml-cuda.cu') (
        @('BC250_HIP_MAX_ALLOC_BYTES') | Where-Object { $without -notcontains $_ })
    Set-Content (Join-Path $root 'ggml/src/ggml-cuda/vendors/hip.h') (
        @('defined(__gfx1013__)') | Where-Object { $without -notcontains $_ })
    return $root
}
$source = New-FixtureSource (Join-Path $out 'source') @()
# The source sets that must be refused, one per missing change of the complete set. Before the set was
# complete in this directory, the first of these built without a word and produced a binary whose
# device code disagreed with the host about the launch geometry of the MoE expert kernel.
$incomplete = @(
    @{ name = 'no-rdna1';  marker = 'defined(__gfx1013__)' }
    @{ name = 'no-dp4a';   marker = '!defined(BC250_HIP_NO_ASM_DP4A)' }
    @{ name = 'no-printf'; marker = '#ifndef BC250_HIP_NO_DEVICE_PRINTF' }
    @{ name = 'no-cap';    marker = 'BC250_HIP_MAX_ALLOC_BYTES' }
)
$shim=Join-Path $out 'cmake-shim.ps1'
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
$results=@()
foreach($case in $cases){
    $build=Join-Path $out $case.name
    [void][IO.Directory]::CreateDirectory($build)
    @('GGML_CUDA_FA:BOOL=OFF','GGML_CUDA_FA_QUANTS:STRING=q4_0','GGML_CUDA_FA_ALL_QUANTS:BOOL=OFF',$case.existing)|Set-Content (Join-Path $build 'CMakeCache.txt')
    $cache=Join-Path $build 'original.txt'
    @('GGML_CUDA_FA:BOOL=OFF','GGML_CUDA_FA_QUANTS:STRING=q4_0','GGML_CUDA_FA_ALL_QUANTS:BOOL=OFF','GGML_HIP:BOOL=ON','GGML_NATIVE:BOOL=OFF','LLAMA_BUILD_COMMON:BOOL=OFF','LLAMA_BUILD_TESTS:BOOL=OFF','LLAMA_BUILD_TOOLS:BOOL=OFF','CMAKE_CXX_FLAGS:STRING=-O2 -DKEEP=123','CMAKE_CXX_FLAGS_RELEASE:STRING=-O3','CMAKE_C_FLAGS:STRING=-DKEEP_C=7','CMAKE_CXX_COMPILER:FILEPATH=X:/compiler.exe',$case.extra)|Set-Content $cache
    $old=$env:ROCM_PATH;$env:ROCM_PATH='host-test-prior'
    $refused=$false;$message=''
    try {& $Builder -Source $source -Build $build -RocmRoot 'fixture-rocm' -OriginalCache $cache -CMake $shim -Bc250AllocationCap:$case.cap -ValidationTools:$case.validation -AssetTools:$case.asset -NoLto:$case.nolto | Out-Null}
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
    }
    $results+=@{name=$case.name;passed=$true;refused=$refused;message=$message}
}
# An incomplete source set is refused before anything is configured, and the refusal names the patch.
foreach($case in $incomplete){
    $build=Join-Path $out ('incomplete-'+$case.name)
    [void][IO.Directory]::CreateDirectory($build)
    $bad=New-FixtureSource (Join-Path $out ('source-'+$case.name)) @($case.marker)
    $cache=Join-Path $build 'original.txt'
    @('GGML_HIP:BOOL=ON','CMAKE_CXX_FLAGS:STRING=')|Set-Content $cache
    $refused=$false;$message=''
    try {& $Builder -Source $bad -Build $build -RocmRoot 'fixture-rocm' -OriginalCache $cache -CMake $shim | Out-Null}
    catch {$refused=$true;$message=$_.Exception.Message}
    if(!$refused){throw "an incomplete source set was accepted: $($case.name)"}
    if($message -notmatch 'is missing 1 of the 4 HIP source changes'){throw "unclear refusal for $($case.name): $message"}
    if(Test-Path (Join-Path $build 'configure-args.json')){throw "refused source set reached CMake: $($case.name)"}
    $results+=@{name='incomplete-'+$case.name;passed=$true;refused=$true;message=$message}
}
# -ApplyPatches turns an unpatched copy of the real upstream file into a complete one, with the real
# patch files and git apply. Only the two files the two new patches touch are needed here, taken from
# the reference source copy and reverse-patched, so the test owns no copy of upstream.
if($ApplyFrom){
    $clean=Join-Path $out 'apply-source'
    [void][IO.Directory]::CreateDirectory((Join-Path $clean 'ggml/src/ggml-cuda/vendors'))
    foreach($rel in @('ggml/src/ggml-cuda/common.cuh','ggml/src/ggml-cuda/ggml-cuda.cu','ggml/src/ggml-cuda/vendors/hip.h')){
        Copy-Item (Join-Path $ApplyFrom $rel) (Join-Path $clean $rel) -Force
    }
    & git -C $clean init -q
    if($LASTEXITCODE){throw 'git init failed in the apply fixture'}
    foreach($item in @('llama-no-device-printf.patch','llama-gfx1013-rdna1.patch','llama-no-asm-dp4a.patch','llama-bc250-allocation-cap.patch')){
        & git -C $clean apply -R -p1 -- (Join-Path $PSScriptRoot $item)
        if($LASTEXITCODE){throw "could not reverse $item out of the reference copy"}
    }
    $build=Join-Path $out 'apply'
    [void][IO.Directory]::CreateDirectory($build)
    $cache=Join-Path $build 'original.txt'
    @('GGML_HIP:BOOL=ON','CMAKE_CXX_FLAGS:STRING=')|Set-Content $cache
    $applied=& $Builder -Source $clean -Build $build -RocmRoot 'fixture-rocm' -OriginalCache $cache -CMake $shim -ApplyPatches
    if($LASTEXITCODE){throw 'the builder failed with -ApplyPatches'}
    if(@($applied|Where-Object{$_ -like 'applied: *'}).Count -ne 4){throw "-ApplyPatches did not apply all four: $($applied -join '; ')"}
    foreach($rel in @('ggml/src/ggml-cuda/common.cuh','ggml/src/ggml-cuda/ggml-cuda.cu','ggml/src/ggml-cuda/vendors/hip.h')){
        $a=(Get-Content (Join-Path $clean $rel) -Raw) -replace "`r`n","`n"
        $b=(Get-Content (Join-Path $ApplyFrom $rel) -Raw) -replace "`r`n","`n"
        if($a -ne $b){throw "after -ApplyPatches, $rel does not match the reference copy"}
    }
    $results+=@{name='apply-patches';passed=$true;refused=$false;message='all four applied; three files match the reference copy'}
}
@{builder_sha256=(Get-FileHash $Builder).Hash;scope='Actual builder with recording CMake shim; no compiler or GPU executed';reference_source=$ApplyFrom;cases=$results}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'results.json')
Write-Output "PASS $($results.Count) builder cases; receipts: $out"
