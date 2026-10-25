# Offline shader-container regression gate. Original DXC signatures are the oracle; no engine or GPU is used.
param([string]$OutputDir, [string]$VsInstall, [ValidateSet('x64','x86')][string]$Arch='x64', [string]$DdiInclude)
$ErrorActionPreference='Stop'
. "$PSScriptRoot/common.ps1"
. "$PSScriptRoot/umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$root=Get-Bc250Root $repo
if(!$OutputDir){$OutputDir=Join-Path $root 'scratch/build/generic-signature-test'}
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
$src=Join-Path $repo 'driver/umd/d3d12/engine-ddi/shader-container'
$dxc=Join-Path $root 'toolchain/nuget/microsoft.windows.sdk.cpp/c/bin/10.0.26100.0/x64/dxc.exe'
$wdk=Join-Path $root 'toolchain/nuget/microsoft.windows.wdk.x64/c/Include/10.0.26100.0/um'
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment $VsInstall $OutputDir $Arch
    $compiler=Resolve-Bc250UmdCompiler $root 'clang-cl' '' $OutputDir $Arch
    $flags=@('/nologo','/std:c++20','/EHsc','/W4','/WX','/O2','/MT','/DNOMINMAX','/external:W0',
        "/external:I$wdk","/external:I$wdk/../shared")
    if($DdiInclude){$flags=@("/external:I$DdiInclude")+$flags}
    $flags=ConvertTo-Bc250ClangFlags $flags $Arch @($repo)
    $sources=@("$src/test/generic-signature-test.cpp","$src/shader-container.cpp","$src/dxil-metadata.cpp")
    $exe=Join-Path $OutputDir 'generic-signature-test.exe'
    & $compiler @flags @sources "/Fo$OutputDir/" "/Fe$exe"
    if($LASTEXITCODE){throw 'generic signature comparator build failed'}
    $cases=@()
    foreach($domain in @('TRI','QUAD','ISOLINE')) {
        foreach($stage in @('hs','ds')) {
            $entry=if($stage -eq 'hs'){'HSMain'}else{'DSMain'}
            $cases+=@{name="$domain-$stage";source='tess';stage=$stage;entry=$entry;profile='6_0';defines=@("TESS_$domain");native=$false;reject=$false}
        }
    }
    foreach($item in @(@('vsps','vs','VSMain'),@('vsps','ps','PSMain'),@('depth','vs','VSMain'),@('depth','ps','PSMain'),
                       @('gs-streams','vs','VSMain'),@('gs-streams','gs','GSMain'),@('cs','cs','CSMain'))) {
        $cases+=@{name=($item[0]+'-'+$item[1]);source=$item[0];stage=$item[1];entry=$item[2];profile='6_0';defines=@('CS50');native=$false;reject=$false}
    }
    foreach($variant in @('MIN16','NATIVE16','INT64','FLOAT64','LESS_EQUAL','GREATER_EQUAL','STANDARD_DEPTH')) {
        foreach($stage in @('vs','ps')) {
            $source=if($variant -in @('LESS_EQUAL','GREATER_EQUAL','STANDARD_DEPTH')){'generic-system-values'}else{'generic-widths'}
            $entry=if($stage -eq 'vs'){'VSMain'}else{'PSMain'}
            $cases+=@{name="$variant-$stage";source=$source;stage=$stage;entry=$entry;profile='6_6';defines=@($variant);native=($variant -eq 'NATIVE16');reject=($variant -in @('INT64','FLOAT64'))}
        }
    }
    $accepted=@(); $manifest=@()
    foreach($case in $cases) {
        $output=Join-Path $OutputDir ($case.name+'.cso')
        $shader=Join-Path $src ('test/hlsl/'+$case.source+'.hlsl')
        $arguments=@('-T',($case.stage+'_'+$case.profile),'-E',$case.entry,'-Fo',$output,$shader)
        foreach($define in $case.defines){$arguments+=@('-D',$define)}
        if($case.native){$arguments+='-enable-16bit-types'}
        $log=Join-Path $OutputDir ($case.name+'-dxc.log')
        $ErrorActionPreference='Continue'
        $messages=(& $dxc @arguments 2>&1 | Out-String)
        $code=$LASTEXITCODE
        $ErrorActionPreference='Stop'
        $messages | Set-Content $log
        if($case.reject) {
            if(!$code -or $messages -notmatch 'cannot be used as shader inputs or outputs'){throw "Expected DXC interface-width rejection: $($case.name)"}
        } else {
            if($code){throw "DXC failed: $($case.name), see $log"}
            $accepted+=$output
        }
        $manifest+=@{name=$case.name;argv=$arguments;source_sha256=(Get-FileHash $shader).Hash;expected_rejection=$case.reject;exit_code=$code;
            output_sha256=$(if(!$case.reject){(Get-FileHash $output).Hash}else{$null})}
    }
    @{dxc=$dxc;dxc_sha256=(Get-FileHash $dxc).Hash;compiler=$compiler;compiler_argv=$flags;cases=$manifest} |
        ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDir 'generic-signature-manifest.json')
    & $exe @accepted
    if($LASTEXITCODE){throw 'generic signature comparison failed'}
    Write-Host 'PASS 23 fixtures / 46 raw+container reconstructions and 4 explicit DXC width-rejection controls'
} finally { Restore-ProcessEnvironment $saved }
