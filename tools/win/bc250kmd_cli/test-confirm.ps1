param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\confirm144-tests",[switch]$IgnoreFlushFailure,
      [ValidateSet('clang-cl','msvc')][string]$Compiler='clang-cl',[string]$MsvcReason='',[string]$VsInstall='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path;$env:TEMP=Join-Path $Out 'tmp';$env:TMP=$env:TEMP
# The directory must exist: cl given a TEMP that does not reports D8018 or D8050 and names no path at all.
New-Item -ItemType Directory -Force $Out,$env:TEMP | Out-Null
$source=Get-Content "$repo\tools\win\bc250kmd_cli\bc250kmd_cli.c" -Raw
$first=$source.IndexOf('static int Confirm(void)');$last=$source.IndexOf('// ---- log:',$first)
$actual=$source.Substring($first,$last-$first)
if($IgnoreFlushFailure){$actual=$actual.Replace('    if (error) {', '    error = ERROR_SUCCESS; if (error) {')}
[IO.File]::WriteAllText("$Out\confirm_actual.inc",$actual)
Copy-Item "$repo\tools\win\bc250kmd_cli\test\confirm_test.c" "$Out\test.c" -Force
# The compiler of the shipped CLI (clang-cl through tools\build\umd-compiler.ps1); -Compiler msvc needs -MsvcReason.
. "$repo\tools\build\umd-compiler.ps1"
$saved=$env:INCLUDE,$env:LIB,$env:PATH
try {
  $tool=Set-Bc250ToolEnvironment "$Root\toolchain\nuget" 'x64' '10.0.26100.0' $VsInstall
  $vs=$tool.VsInstall
  $cc=Resolve-Bc250UmdCompiler $Root $Compiler $MsvcReason $Out 'x64' $tool.Cl
  $flags=Get-Bc250ToolFlags $Compiler @('/nologo','/TC','/W4','/WX','/O2','/MT',"/I$repo\driver\state") 'x64' @($repo)
  Invoke-Bc250UmdCompiler $cc @flags "/Fo$Out\" "/Fe$Out\confirm_test.exe" "$Out\test.c"
  if($LASTEXITCODE -ne 0){throw 'CLI test build failed'}
} finally { $env:INCLUDE,$env:LIB,$env:PATH=$saved }
& "$Out\confirm_test.exe";$cliResult=$LASTEXITCODE
$source=Get-Content "$repo\tools\win\bc250mon\src\KmdProvider.cs" -Raw
$first=$source.IndexOf('        public void Confirm()');$last=$source.IndexOf('        public string Summary()',$first)
$actual=$source.Substring($first,$last-$first)
if($IgnoreFlushFailure){$actual=$actual.Replace('state.WriteDword(', 'state.IgnoreWriteDword(')}
$test=Get-Content "$repo\tools\win\bc250mon\test\ConfirmTest.cs" -Raw
[IO.File]::WriteAllText("$Out\ConfirmTest.cs",$test.Replace('// ACTUAL_CONFIRM_METHOD',$actual))
$fx="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
& "$vs\MSBuild\Current\Bin\Roslyn\csc.exe" /nologo /noconfig /nostdlib+ "/reference:$fx\mscorlib.dll" "/reference:$fx\System.dll" /target:exe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 "/out:$Out\monitor-confirm-test.exe" "$Out\ConfirmTest.cs"
if($LASTEXITCODE -ne 0){throw 'Monitor test build failed'}
& "$Out\monitor-confirm-test.exe";$monitorResult=$LASTEXITCODE
if($cliResult -ne 0 -or $monitorResult -ne 0){exit 1}
