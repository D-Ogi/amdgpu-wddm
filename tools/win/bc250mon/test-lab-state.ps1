param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Out | Out-Null
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Web.Extensions.dll' |
    ForEach-Object { "/reference:$fx\$_" }
# The real providers, so the marker file names and the panel orders under test are the deployed ones.
$sources = 'State.cs', 'Driver.cs', 'Providers.cs', 'KmdInfoProvider.cs', 'GraphicsPipelineProvider.cs',
           'GraphicsApiProvider.cs', 'TelemetryProvider.cs', 'LabState.cs', 'OperatingPointProvider.cs',
           'MeasurementGuardProvider.cs' | ForEach-Object { "$here\src\$_" }
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 `
    "/out:$Out\lab-state-test.exe" @sources "$here\test\LabStateTest.cs"
if ($LASTEXITCODE -ne 0) { throw 'Lab state check compilation failed' }
& "$Out\lab-state-test.exe" $Out
if ($LASTEXITCODE -ne 0) { throw 'Lab state check failed' }
