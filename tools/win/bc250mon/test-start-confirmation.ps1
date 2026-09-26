param([Parameter(Mandatory)][string]$Out, [string]$PolicySource)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $PolicySource) { $PolicySource = "$here\src\StartConfirmationPolicy.cs" }
New-Item -ItemType Directory -Force $Out | Out-Null
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Web.Extensions.dll' | ForEach-Object { "/reference:$fx\$_" }
& python "$here\test\generate-start-confirmation-test.py" --out "$Out\StartConfirmationActual.cs"
if ($LASTEXITCODE -ne 0) { throw 'Provider extraction failed' }
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 "/out:$Out\start-confirmation-test.exe" "$here\src\Driver.cs" $PolicySource "$Out\StartConfirmationActual.cs"
if ($LASTEXITCODE -ne 0) { throw 'Start confirmation check compilation failed' }
& "$Out\start-confirmation-test.exe"
if ($LASTEXITCODE -ne 0) { throw 'Start confirmation check failed' }
