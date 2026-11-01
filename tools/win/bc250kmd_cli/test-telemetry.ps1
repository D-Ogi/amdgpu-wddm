param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\telemetry-client-tests", [switch]$BypassBoardAdmission,
      [ValidateSet('clang-cl','msvc')][string]$Compiler='clang-cl',[string]$MsvcReason='',[string]$VsInstall='')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path;$env:TEMP=Join-Path $Out 'tmp';$env:TMP=$env:TEMP
# The directory must exist: cl given a TEMP that does not reports D8018 or D8050 and names no path at all.
New-Item -ItemType Directory -Force $Out,$env:TEMP | Out-Null
$source=Get-Content "$repo\tools\win\bc250kmd_cli\bc250kmd_cli.c" -Raw
$first=$source.IndexOf('#ifndef BC250_ESCAPE_RUN_DPM')
$last=$source.IndexOf('// ---- telemetry: adapter access',$first)
if($first -lt 0 -or $last -lt 0){throw 'Telemetry block not found in bc250kmd_cli.c'}
$actual=$source.Substring($first,$last-$first)
if ($BypassBoardAdmission) {
    $pattern = '(?s)static NTSTATUS BoardCapabilityRequired\(ULONG flag\)\s*\{.*?\n\}'
    if ([regex]::Matches($actual, $pattern).Count -ne 1) { throw 'Board admission mutation anchor changed' }
    $actual = [regex]::Replace($actual, $pattern, 'static NTSTATUS BoardCapabilityRequired(ULONG flag) { (void)flag; return 0; }')
}
$template=Get-Content "$repo\tools\win\bc250kmd_cli\test\telemetry_test.c" -Raw
[IO.File]::WriteAllText("$Out\test.c",$template.Replace('// ACTUAL_TELEMETRY',$actual))
Copy-Item "$repo\driver\kmd\bc250kmd_escape.h" "$Out\bc250kmd_escape.h" -Force
# The compiler of the shipped CLI and control DLL (clang-cl through tools\build\umd-compiler.ps1); -Compiler msvc
# needs -MsvcReason. The board provider below is KMD code, which the WDK compiler builds: it stays on cl.
. "$repo\tools\build\umd-compiler.ps1"
$saved=$env:INCLUDE,$env:LIB,$env:PATH
try {
  $tool=Set-Bc250ToolEnvironment "$Root\toolchain\nuget" 'x64' '10.0.26100.0' $VsInstall
  $cc=Resolve-Bc250UmdCompiler $Root $Compiler $MsvcReason $Out 'x64' $tool.Cl
  $flags=Get-Bc250ToolFlags $Compiler @('/nologo','/TC','/W4','/WX','/O2','/MT') 'x64' @($repo)
  Invoke-Bc250UmdCompiler $cc @flags "/Fo$Out\" "/Fe$Out\telemetry_test.exe" "$Out\test.c"
  if($LASTEXITCODE -ne 0){throw 'Telemetry client test build failed'}
} finally { $env:INCLUDE,$env:LIB,$env:PATH=$saved }
& "$repo\driver\kmd\test\run_board_provider.ps1" -Root $Root -Out "$Out\provider" -VsInstall $tool.VsInstall
$result = @(& "$Out\telemetry_test.exe" "$Out\provider\board-caps.bin")
$code = $LASTEXITCODE
$result | Set-Content "$Out\result.txt"
$result | Write-Output
if ($BypassBoardAdmission) {
    if ($code -ne 1 -or -not ($result -match '^FAIL line')) { throw 'Board admission mutation did not fail runtime checks' }
} elseif ($code -ne 0) { throw 'Telemetry client tests failed' }
