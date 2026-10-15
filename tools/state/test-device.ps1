# Actual identity/activation bodies and StartDevice admission prefix. CPU only.
param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits,
      [ValidateSet('x64','x86')][string]$Arch='x64',
      [ValidateSet('none','activate-during-validation','skip-start-activation')][string]$Mutation='none')
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
    $files=@('driver/kmd/state_device.c','driver/kmd/state_kernel.h','driver/kmd/pnp.c',
        'driver/state/identity.c','driver/state/identity.h','driver/state/state.h','driver/state/transfer.h',
        'driver/state/device_test.c','tools/state/test-device.ps1')
    $hashes=@{}
    foreach($file in $files){$hashes[$file]=(Get-FileHash (Join-Path $repo $file) -Algorithm SHA256).Hash}
    $device=[IO.File]::ReadAllText("$repo\driver\kmd\state_device.c")
    $device=[regex]::Replace($device,'(?m)^#include[^\r\n]*','')
    $pnp=[IO.File]::ReadAllText("$repo\driver\kmd\pnp.c")
    $begin=$pnp.IndexOf('NTSTATUS Bc250StartDevice(')
    if($begin -lt 0){throw 'StartDevice declaration missing'}
    $last='GuardStage(StageStartGuardPassed);'
    $end=$pnp.IndexOf($last,$begin)
    if($end -lt 0){throw 'StartDevice admission boundary missing'}
    $prefix=$pnp.Substring($begin,$end+$last.Length-$begin)
    if($prefix -notmatch 'GuardCheckAndCountStart' -or $prefix -notmatch 'DriverStateActivateDevice'){
        throw 'StartDevice prefix does not contain both state activation and budget admission'
    }
    # Preserve every production statement up to the first admitted hardware path.
    # The remainder has hardware dependencies and is deliberately not executed.
    $prefix+="`n    (void)DxgkStartInfo;(void)DxgkInterface;return HardwareBoundary();`n}`n"
    if($Mutation -eq 'activate-during-validation'){
        $pattern='(?s)NTSTATUS DriverStateValidateDevice\(PDEVICE_OBJECT device\)\s*\{.*?\n\}'
        if([regex]::Matches($device,$pattern).Count -ne 1){throw 'Unique validation function mutation target missing'}
        $device=[regex]::Replace($device,$pattern,'NTSTATUS DriverStateValidateDevice(PDEVICE_OBJECT device) { return DriverStateActivateDevice(device); }')
    } elseif($Mutation -eq 'skip-start-activation'){
        $pattern='status\s*=\s*DriverStateActivateDevice\(device->PhysicalDeviceObject\);'
        if([regex]::Matches($prefix,$pattern).Count -ne 1){throw 'Unique StartDevice activation mutation target missing'}
        $prefix=[regex]::Replace($prefix,$pattern,'status=STATUS_SUCCESS;')
    }
    $template=[IO.File]::ReadAllText("$repo\driver\state\device_test.c")
    foreach($marker in '/* ACTUAL_DEVICE */','/* ACTUAL_START_PREFIX */'){
        if($template.Split(@($marker),[StringSplitOptions]::None).Count -ne 2){throw "Invalid insertion marker $marker"}
    }
    $generated="$Out\device_actual.c"
    [IO.File]::WriteAllText($generated,$template.Replace('/* ACTUAL_DEVICE */',$device).Replace('/* ACTUAL_START_PREFIX */',$prefix))
    $vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    if($LASTEXITCODE){throw 'Toolchain discovery failed'}
    $msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
    $sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
    $libs=Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
    $bin="$($msvc.FullName)\bin\Hostx64\$Arch"
    $env:PATH="$bin;$env:PATH"
    $env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
    $env:LIB="$($msvc.FullName)\lib\$Arch;$libs\ucrt\$Arch;$libs\um\$Arch"
    $exe="$Out\device_test.exe"
    $arguments=@('/nologo','/TC','/W4','/WX','/O2','/MT',"/I$repo\driver\state","/Fo$Out\","/Fe$exe",
        "$repo\driver\state\identity.c",$generated,'/link','bcrypt.lib')
    $build=& "$bin\cl.exe" @arguments 2>&1
    $buildExit=$LASTEXITCODE
    $build | Set-Content -Encoding UTF8 "$Out\build.log"
    if($buildExit){throw "Device fixture compile failed: see $Out\build.log"}
    $imports=& "$bin\dumpbin.exe" /imports $exe
    if($LASTEXITCODE){throw 'Device fixture import inspection failed'}
    $imports | Set-Content -Encoding UTF8 "$Out\imports.log"
    if(($imports -join "`n") -match '\b((?:Zw|Nt)[A-Za-z0-9]*|Reg(?:Open|Create|Query|Set|Delete|Flush)[A-Za-z0-9]*|OpenSCManagerW|OpenServiceW|CreateThread|CreateRemoteThread|CreateProcess[AW]|DeviceIoControl|D3D11CreateDevice|D3D12CreateDevice)\b'){
        throw 'Device fixture unexpectedly imports an installed-state, thread/process or GPU API'
    }
    $output=& $exe
    $testExit=$LASTEXITCODE
    $output | Set-Content -Encoding UTF8 "$Out\test.log"
    foreach($file in $files){
        if((Get-FileHash (Join-Path $repo $file) -Algorithm SHA256).Hash -ne $hashes[$file]){throw "Input changed during fixture: $file"}
    }
    $wanted=0;if($Mutation -ne 'none'){$wanted=1}
    [ordered]@{arch=$Arch;source_hashes=$hashes;compile_arguments=$arguments;compile_exit=$buildExit;
        generated_sha256=(Get-FileHash $generated -Algorithm SHA256).Hash;
        exe_sha256=(Get-FileHash $exe -Algorithm SHA256).Hash;
        mutation=$Mutation;test_exit=$testExit;expected_test_exit=$wanted;test_output=($output -join "`n");
        actual_start_prefix_ends_after=$last;system_side_effect_imports_absent=$true;source_inputs_unchanged=$true;
        environment='Explicit system/toolchain allow-list; TEMP/TMP under output';gpu_or_lab_used=$false} |
        ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 "$Out\receipt.json"
    $output
    if($testExit -ne $wanted){throw "Device fixture test exit $testExit; expected $wanted"}
    if($Mutation -eq 'activate-during-validation' -and ($output -join "`n") -notmatch 'FAIL valid validation stays read-only'){
        throw 'Mutation did not exercise side-effect-free validation assertions'
    }
    if($Mutation -eq 'skip-start-activation' -and ($output -join "`n") -notmatch 'FAIL Start activation before guard'){
        throw 'Mutation did not exercise pre-budget failure assertions'
    }
} finally {
    foreach($name in @([Environment]::GetEnvironmentVariables('Process').Keys)){
        [Environment]::SetEnvironmentVariable($name,$null,'Process')
    }
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
