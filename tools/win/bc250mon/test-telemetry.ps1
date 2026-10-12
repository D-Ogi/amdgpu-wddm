param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Out | Out-Null
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Web.Extensions.dll' | ForEach-Object { "/reference:$fx\$_" }
# LabState.cs holds the DpmFeed TelemetryProvider publishes into (one snapshot, three panels).
$sources = 'State.cs', 'Driver.cs', 'Providers.cs', 'SystemMetrics.cs', 'LabState.cs', 'TelemetryProvider.cs' | ForEach-Object { "$here\src\$_" }
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 "/out:$Out\telemetry-test.exe" @sources "$here\test\TelemetryTest.cs"
if ($LASTEXITCODE -ne 0) { throw 'Telemetry check compilation failed' }
& "$Out\telemetry-test.exe" $Out
if ($LASTEXITCODE -ne 0) { throw 'Telemetry check failed' }
