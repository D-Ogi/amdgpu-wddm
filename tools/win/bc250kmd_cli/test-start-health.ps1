param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\start-health-client-tests",[switch]$IdleRead,
      [ValidateSet('clang-cl','msvc')][string]$Compiler='clang-cl',[string]$MsvcReason='',[string]$VsInstall='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path;$env:TEMP=Join-Path $Out 'tmp';$env:TMP=$env:TEMP
# The directory must exist: cl given a TEMP that does not reports D8018 or D8050 and names no path at all.
New-Item -ItemType Directory -Force $Out,$env:TEMP | Out-Null
$source=Get-Content "$repo\tools\win\bc250kmd_cli\bc250kmd_cli.c" -Raw
$first=$source.IndexOf('BC250_CONTROL_API LONG WINAPI Bc250StartHealth(')
$last=$source.IndexOf('static int StartHealth(',$first)
$actual=$source.Substring($first,$last-$first)
# -IdleRead: the mutation control. The export sends both operations with NoAdapterSynchronization alone
# (0.7.213); with HardwareAccess instead each one idles the adapter, and the test must say so.
if($IdleRead){
  $mutant=$actual.Replace(',sizeof(*data),1,&status))return status;', ',sizeof(*data),0,&status))return status;')
  if($mutant -eq $actual){throw 'the -IdleRead mutation no longer applies: the export no longer sends softwareOnly 1'}
  $actual=$mutant
}
$template=Get-Content "$repo\tools\win\bc250kmd_cli\test\start_health_test.c" -Raw
[IO.File]::WriteAllText("$Out\test.c",$template.Replace('// ACTUAL_START_HEALTH',$actual))
Copy-Item "$repo\driver\kmd\bc250kmd_escape.h" "$Out\bc250kmd_escape.h" -Force
# The compiler of the shipped CLI (clang-cl through tools\build\umd-compiler.ps1); -Compiler msvc needs -MsvcReason.
. "$repo\tools\build\umd-compiler.ps1"
$saved=$env:INCLUDE,$env:LIB,$env:PATH
try {
  $tool=Set-Bc250ToolEnvironment "$Root\toolchain\nuget" 'x64' '10.0.26100.0' $VsInstall
  $cc=Resolve-Bc250UmdCompiler $Root $Compiler $MsvcReason $Out 'x64' $tool.Cl
  $flags=Get-Bc250ToolFlags $Compiler @('/nologo','/TC','/W4','/WX','/O2','/MT') 'x64' @($repo)
  Invoke-Bc250UmdCompiler $cc @flags "/Fo$Out\" "/Fe$Out\start_health_test.exe" "$Out\test.c"
  if($LASTEXITCODE -ne 0){throw 'CLI test build failed'}
} finally { $env:INCLUDE,$env:LIB,$env:PATH=$saved }
& "$Out\start_health_test.exe"
if($LASTEXITCODE -ne 0){throw 'Start health client tests failed'}
