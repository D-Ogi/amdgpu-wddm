param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\confirm144-tests",[switch]$IgnoreFlushFailure)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path;$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
$source=Get-Content "$repo\tools\win\bc250kmd_cli\bc250kmd_cli.c" -Raw
$first=$source.IndexOf('static int Confirm(void)');$last=$source.IndexOf('// ---- log:',$first)
$actual=$source.Substring($first,$last-$first)
if($IgnoreFlushFailure){$actual=$actual.Replace('s = RegFlushKey(key);','(void)RegFlushKey(key); s = ERROR_SUCCESS;')}
[IO.File]::WriteAllText("$Out\confirm_actual.inc",$actual)
Copy-Item "$repo\tools\win\bc250kmd_cli\test\confirm_test.c" "$Out\test.c" -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /O2 /MT "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/I$sdk\Include\10.0.26100.0\um" "/I$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\confirm_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'CLI test build failed'}
& "$Out\confirm_test.exe";$cliResult=$LASTEXITCODE
$source=Get-Content "$repo\tools\win\bc250mon\src\KmdProvider.cs" -Raw
$first=$source.IndexOf('        public void Confirm()');$last=$source.IndexOf('        public string Summary()',$first)
$actual=$source.Substring($first,$last-$first)
if($IgnoreFlushFailure){$actual=$actual.Replace('if (error != 0)','if (error == int.MinValue)')}
$test=Get-Content "$repo\tools\win\bc250mon\test\ConfirmTest.cs" -Raw
[IO.File]::WriteAllText("$Out\ConfirmTest.cs",$test.Replace('// ACTUAL_CONFIRM_METHOD',$actual))
$fx="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
& "$vs\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /noconfig /nostdlib+ "/reference:$fx\mscorlib.dll" "/reference:$fx\System.dll" /target:exe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 "/out:$Out\monitor-confirm-test.exe" "$Out\ConfirmTest.cs"
if($LASTEXITCODE -ne 0){throw 'Monitor test build failed'}
& "$Out\monitor-confirm-test.exe";$monitorResult=$LASTEXITCODE
if($cliResult -ne 0 -or $monitorResult -ne 0){exit 1}
