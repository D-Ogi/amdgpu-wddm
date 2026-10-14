param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference='Stop'
$here=$PSScriptRoot
New-Item -ItemType Directory -Force $Out | Out-Null
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc=Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx="$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs='mscorlib.dll','System.dll','System.Core.dll','System.Drawing.dll','System.Windows.Forms.dll','System.Web.Extensions.dll' |
    ForEach-Object { "/reference:$fx\$_" }
# Compile the production sources, but enter only the test's Main. Every state
# call uses the fake session; Program.Main, providers, UI and GPU calls never run.
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /main:Bc250Mon.StateTest /platform:x64 /optimize+ /warnaserror+ `
    /langversion:7.3 "/out:$Out\state-test.exe" (Get-ChildItem "$here\src\*.cs").FullName "$here\test\StateTest.cs"
if($LASTEXITCODE -ne 0){throw 'Overlay state test compilation failed'}
& "$Out\state-test.exe"
if($LASTEXITCODE -ne 0){throw 'Overlay state test failed'}
