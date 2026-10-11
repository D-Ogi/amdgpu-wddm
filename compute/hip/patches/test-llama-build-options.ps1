# Invoke the actual builder through a recording CMake fixture. No compiler or GPU runs.
param(
    [Parameter(Mandatory=$true)][string]$Out,
    [string]$Builder = ''
)
$ErrorActionPreference='Stop'
if ([string]::IsNullOrWhiteSpace($Builder)) {
    $Builder = Join-Path $PSScriptRoot 'build-llama-patched.ps1'
}
$out=Join-Path $Out ('builder-test-'+[guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($out)
$source=Join-Path $out 'source'
[void][IO.Directory]::CreateDirectory((Join-Path $source 'ggml/src/ggml-cuda'))
Set-Content (Join-Path $source 'ggml/src/ggml-cuda/common.cuh') '#ifndef BC250_HIP_NO_DEVICE_PRINTF'
Set-Content (Join-Path $source 'ggml/src/ggml-cuda/ggml-cuda.cu') 'BC250_HIP_MAX_ALLOC_BYTES'
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
@{builder_sha256=(Get-FileHash $Builder).Hash;scope='Actual builder with recording CMake shim; no compiler or GPU executed';cases=$results}|ConvertTo-Json -Depth 5|Set-Content (Join-Path $out 'results.json')
Write-Output "PASS $($results.Count) builder cases; receipts: $out"
