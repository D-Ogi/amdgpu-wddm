# Actual kernel bootstrap and commit bodies on one CPU-only registry model.
param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits,
      [ValidateSet('x64','x86')][string]$Arch='x64',[switch]$IgnoreFirstFlushError)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
$Out=[IO.Path]::GetFullPath($Out)
$saved=@{}
foreach($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()){$saved[$entry.Key]=$entry.Value}
try {
    $keep=@('SystemRoot','WINDIR','ComSpec','ProgramFiles','ProgramFiles(x86)','ProgramW6432',
        'ProgramData','PROCESSOR_ARCHITECTURE','PROCESSOR_IDENTIFIER','NUMBER_OF_PROCESSORS','OS','SystemDrive')
    foreach($name in @($saved.Keys)){
        if($name -notin $keep){[Environment]::SetEnvironmentVariable($name,$null,'Process')}
    }
    New-Item -ItemType Directory -Force "$Out\tmp" | Out-Null
    $env:TEMP="$Out\tmp";$env:TMP=$env:TEMP
    $env:PATH="$env:SystemRoot\System32;$env:SystemRoot"
    $kernel=[IO.File]::ReadAllText("$repo\driver\kmd\state_kernel.c")
    $transaction=[IO.File]::ReadAllText("$repo\driver\kmd\state_transaction.c")
    $kernel=[regex]::Replace($kernel,'(?m)^#include[^\r\n]*','')
    $transaction=[regex]::Replace($transaction,'(?m)^#include[^\r\n]*','')
    if($IgnoreFirstFlushError){
        $old='status=ZwFlushKey(t->generation);if(!NT_SUCCESS(status)) return status;'
        if($transaction.Split(@($old),[StringSplitOptions]::None).Count -ne 2){throw 'Missing unique first-flush mutation target'}
        $transaction=$transaction.Replace($old,'(void)ZwFlushKey(t->generation);status=STATUS_SUCCESS;')
    }
    $template=[IO.File]::ReadAllText("$repo\driver\state\lifecycle_test.c")
    foreach($marker in '/* ACTUAL_KERNEL */','/* ACTUAL_TRANSACTION */'){
        if($template.Split(@($marker),[StringSplitOptions]::None).Count -ne 2){throw "Invalid insertion marker $marker"}
    }
    $generated="$Out\lifecycle_actual.c"
    [IO.File]::WriteAllText($generated,$template.Replace('/* ACTUAL_KERNEL */',$kernel).Replace('/* ACTUAL_TRANSACTION */',$transaction))
    $vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    if($LASTEXITCODE){throw 'Toolchain discovery failed'}
    $msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
    $sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
    $libs=Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
    $bin="$($msvc.FullName)\bin\Hostx64\$Arch"
    $env:PATH="$bin;$env:PATH"
    $env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
    $env:LIB="$($msvc.FullName)\lib\$Arch;$libs\ucrt\$Arch;$libs\um\$Arch"
    $exe="$Out\lifecycle_test.exe"
    $arguments=@('/nologo','/TC','/W4','/WX','/O2','/MT',"/I$repo\driver\state","/Fo$Out\","/Fe$exe",
        "$repo\driver\state\state.c","$repo\driver\state\transfer.c",$generated,'/link','advapi32.lib','authz.lib','ntdll.lib')
    $build=& "$bin\cl.exe" @arguments 2>&1
    $buildExit=$LASTEXITCODE
    $build | Set-Content -Encoding UTF8 "$Out\build.log"
    if($buildExit){throw "Lifecycle compile failed: see $Out\build.log"}
    $imports=& "$bin\dumpbin.exe" /imports $exe
    if($LASTEXITCODE){throw 'Lifecycle import inspection failed'}
    $imports | Set-Content -Encoding UTF8 "$Out\imports.log"
    if(($imports -join "`n") -match '\b((?:Zw|Nt)(?:CreateKey|OpenKey|QueryKey|QueryValueKey|SetValueKey|DeleteValueKey|FlushKey)|Reg(?:Open|Create|Query|Set|Delete|Flush)[A-Za-z0-9]*|OpenSCManagerW|OpenServiceW)\b'){
        throw 'Lifecycle fixture unexpectedly imports a real registry/SCM API'
    }
    $output=& $exe
    $testExit=$LASTEXITCODE
    $output | Set-Content -Encoding UTF8 "$Out\test.log"
    $hashes=@{}
    foreach($file in 'driver/kmd/state_kernel.c','driver/kmd/state_transaction.c','driver/state/state.c',
        'driver/state/state.h','driver/state/catalog.generated.h','driver/state/transfer.c','driver/state/transfer.h',
        'driver/state/lifecycle_test.c','tools/state/test-lifecycle.ps1'){
        $hashes[$file]=(Get-FileHash (Join-Path $repo $file) -Algorithm SHA256).Hash
    }
    $wanted=0;if($IgnoreFirstFlushError){$wanted=1}
    [ordered]@{arch=$Arch;source_hashes=$hashes;compile_arguments=$arguments;compile_exit=$buildExit;
        generated_sha256=(Get-FileHash $generated -Algorithm SHA256).Hash;
        exe_sha256=(Get-FileHash $exe -Algorithm SHA256).Hash;
        mutation=[bool]$IgnoreFirstFlushError;test_exit=$testExit;expected_test_exit=$wanted;
        test_output=($output -join "`n");real_registry_scm_imports_absent=$true;
        environment='Explicit system/toolchain allow-list; TEMP/TMP under output';gpu_or_lab_used=$false} |
        ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 "$Out\receipt.json"
    # A mutation control must fail; its FAIL lines are labelled so that nobody reads them as a defect.
    if($IgnoreFirstFlushError){$output | ForEach-Object {"expected under mutation ignore-first-flush-error: $_"}} else {$output}
    if($testExit -ne $wanted){throw "Lifecycle test exit $testExit; expected $wanted"}
    if($IgnoreFirstFlushError -and ($output -join "`n") -notmatch 'DriverStateActivate\(good\)==STATUS_REGISTRY_IO_FAILED'){
        throw 'Mutation did not exercise the first-flush propagation assertion'
    }
    if($IgnoreFirstFlushError){'mutation control ignore-first-flush-error: caught (fixture exit 1, first-flush assertion failed as required)'}
} finally {
    foreach($name in @([Environment]::GetEnvironmentVariables('Process').Keys)){
        [Environment]::SetEnvironmentVariable($name,$null,'Process')
    }
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
