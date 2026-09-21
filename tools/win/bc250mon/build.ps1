# Builds bc250mon.exe for .NET Framework 4.8 (part of Windows, nothing to install on the target) with the
# C# compiler that ships with Visual Studio.   pwsh tools\win\bc250mon\build.ps1 -Out P:\BC-250\scratch\build\bc250mon

param([Parameter(Mandatory)][string]$Out)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
New-Item -ItemType Directory -Force $Out | Out-Null

$refs = 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.Web.Extensions.dll' |
    ForEach-Object { "/reference:$fx\$_" }
& $csc /nologo /noconfig /nostdlib+ "/reference:$fx\mscorlib.dll" @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ `
    /langversion:7.3 "/win32manifest:$here\app.manifest" "/out:$Out\bc250mon.exe" (Get-ChildItem "$here\src\*.cs").FullName
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Get-Item "$Out\bc250mon.exe" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
