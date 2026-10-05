param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$here=Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Out | Out-Null
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc=Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs='mscorlib.dll','System.dll','System.Core.dll','System.Web.Extensions.dll' | ForEach-Object {"/reference:$fx\$_"}
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 "/out:$Out\graphics-summary-test.exe" "$here\src\State.cs" "$here\src\GraphicsPipelineProvider.cs" "$here\test\GraphicsSummaryTest.cs"
if($LASTEXITCODE -ne 0){throw 'Summary pause test compilation failed'}
& "$Out\graphics-summary-test.exe" $Out
if($LASTEXITCODE -ne 0){throw 'Summary pause test failed'}
