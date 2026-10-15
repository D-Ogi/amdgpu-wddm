# Compile the real service-root boundaries against fake APIs; no SCM/registry calls.
param([Parameter(Mandatory=$true)][string]$Out,[Parameter(Mandatory=$true)][string]$Kits,
      [ValidateSet('x64','x86')][string]$Arch='x64',
      [ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$SourceRevision='',
      [switch]$ExpectFailure)
$ErrorActionPreference='Stop'
if($ExpectFailure -and -not $SourceRevision){throw 'Expected failure requires a pinned source revision'}
$repo=(Resolve-Path "$PSScriptRoot\..\..").Path
$Out=[IO.Path]::GetFullPath($Out)
$git=(Get-Command git -ErrorAction Stop).Source
$saved=@{}
foreach($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()){$saved[$entry.Key]=$entry.Value}
function Function-Text([string]$Source,[string]$Name) {
    $match=[regex]::Match($Source,'\b(?:static\s+|API\s+)DWORD\s+(?:WINAPI\s+)?'+[regex]::Escape($Name)+'\s*\([^;{}]*\)\s*\{')
    if(-not $match.Success){throw "Missing actual function $Name"}
    $depth=1;$end=$match.Index+$match.Length
    while($depth -and $end -lt $Source.Length){
        if($Source[$end] -eq '{'){$depth++}
        elseif($Source[$end] -eq '}'){$depth--}
        $end++
    }
    if($depth){throw "Unclosed actual function $Name"}
    return $Source.Substring($match.Index,$end-$match.Index)
}
function Text-Hash([string]$Text) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text)))).Replace('-','')}
    finally{$hash.Dispose()}
}
try {
    $keep=@('SystemRoot','WINDIR','ComSpec','ProgramFiles','ProgramFiles(x86)','ProgramW6432',
        'ProgramData','PROCESSOR_ARCHITECTURE','PROCESSOR_IDENTIFIER','NUMBER_OF_PROCESSORS','OS','SystemDrive')
    foreach($name in @($saved.Keys)){
        if($name -notin $keep){[Environment]::SetEnvironmentVariable($name,$null,'Process')}
    }
    New-Item -ItemType Directory -Force "$Out\tmp" | Out-Null
    $env:TEMP="$Out\tmp";$env:TMP=$env:TEMP
    $env:PATH="$env:SystemRoot\System32;$env:SystemRoot"
    $sourceFiles=@('driver/state/installer_worker.c','driver/state/windows.c','driver/state/client.c')
    $sources=@();$sourceHashes=@{}
    foreach($file in $sourceFiles){
        if($SourceRevision){
            $lines=& $git -C $repo show "${SourceRevision}:$file"
            if($LASTEXITCODE){throw "Cannot read pinned source $file"}
            $text=$lines -join "`n"
        } else {$text=[IO.File]::ReadAllText((Join-Path $repo $file))}
        $sources+=,$text;$sourceHashes[$file]=Text-Hash $text
    }
    $origin=Function-Text $sources[0] 'Origin'
    $shared=Function-Text $sources[1] 'SharedRoot'
    $directory=Function-Text $sources[2] 'AmdgpuClientLogDirectory'
    $template=[IO.File]::ReadAllText("$repo\driver\state\service_root_test.c")
    if($template.Split(@('/* ACTUAL_ORIGIN */'),[StringSplitOptions]::None).Count -ne 2 -or
       $template.Split(@('/* ACTUAL_SHARED_ROOT */'),[StringSplitOptions]::None).Count -ne 2 -or
       $template.Split(@('/* ACTUAL_LOG_DIRECTORY */'),[StringSplitOptions]::None).Count -ne 2){throw 'Invalid test insertion markers'}
    $generated="$Out\service_root_actual.c"
    [IO.File]::WriteAllText($generated,$template.Replace('/* ACTUAL_ORIGIN */',$origin).Replace('/* ACTUAL_SHARED_ROOT */',$shared).Replace('/* ACTUAL_LOG_DIRECTORY */',$directory))
    $vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
    if($LASTEXITCODE){throw 'Toolchain discovery failed'}
    $msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
    $sdk=Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
    $libs=Join-Path $Kits "microsoft.windows.sdk.cpp.$Arch\c"
    $bin="$($msvc.FullName)\bin\Hostx64\$Arch"
    $env:PATH="$bin;$env:PATH"
    $env:INCLUDE="$($msvc.FullName)\include;$sdk\Include\10.0.26100.0\ucrt;$sdk\Include\10.0.26100.0\shared;$sdk\Include\10.0.26100.0\um"
    $env:LIB="$($msvc.FullName)\lib\$Arch;$libs\ucrt\$Arch;$libs\um\$Arch"
    $exe="$Out\service_root_test.exe"
    $arguments=@('/nologo','/TC','/W4','/WX','/O2','/MT',"/I$repo\driver\state","/Fo$Out\","/Fe$exe",$generated)
    $build=& "$bin\cl.exe" @arguments 2>&1
    $buildExit=$LASTEXITCODE
    $build | Set-Content -Encoding UTF8 "$Out\build.log"
    if($buildExit){throw "Service root test compile failed: see $Out\build.log"}
    $imports=& "$bin\dumpbin.exe" /imports $exe
    if($LASTEXITCODE){throw 'Service root import inspection failed'}
    $imports | Set-Content -Encoding UTF8 "$Out\imports.log"
    if(($imports -join "`n") -match '\b(OpenSCManagerW|OpenServiceW|GetSharedServiceRegistryStateKey|GetSharedServiceDirectory|RegOpenKeyExW|RegCloseKey|CloseServiceHandle)\b'){
        throw 'Service root fixture unexpectedly imports a real SCM/registry API'
    }
    $output=& $exe
    $testExit=$LASTEXITCODE
    $output | Set-Content -Encoding UTF8 "$Out\test.log"
    $wanted=0;if($ExpectFailure){$wanted=1}
    $receipt=[ordered]@{
        arch=$Arch;source_revision=$SourceRevision;source_text_utf8_sha256=$sourceHashes;
        extracted_origin_sha256=(Text-Hash $origin);extracted_shared_root_sha256=(Text-Hash $shared);
        extracted_log_directory_sha256=(Text-Hash $directory);
        template_sha256=(Get-FileHash "$repo\driver\state\service_root_test.c" -Algorithm SHA256).Hash;
        launcher_sha256=(Get-FileHash $PSCommandPath -Algorithm SHA256).Hash;
        generated_sha256=(Get-FileHash $generated -Algorithm SHA256).Hash;
        exe_sha256=(Get-FileHash $exe -Algorithm SHA256).Hash;
        compile_arguments=$arguments;compile_exit=$buildExit;test_exit=$testExit;expected_test_exit=$wanted;
        test_output=($output -join "`n");real_registry_scm_imports_absent=$true;
        environment='Explicit system/toolchain allow-list; TEMP/TMP under output';gpu_or_lab_used=$false
    }
    $receipt | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 "$Out\receipt.json"
    $output
    if($testExit -ne $wanted){throw "Service root test exit $testExit; expected $wanted"}
    if($ExpectFailure -and ($output -join "`n") -notmatch 'access == REQUIRED_SERVICE_ACCESS'){
        throw 'Old-source failure did not exercise the service access regression'
    }
} finally {
    foreach($name in @([Environment]::GetEnvironmentVariables('Process').Keys)){
        [Environment]::SetEnvironmentVariable($name,$null,'Process')
    }
    foreach($name in $saved.Keys){[Environment]::SetEnvironmentVariable($name,$saved[$name],'Process')}
}
