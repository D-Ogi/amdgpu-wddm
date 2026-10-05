# Builds bc250mon.exe for .NET Framework 4.8 (part of Windows, nothing to install on the target) with the
# C# compiler that ships with Visual Studio.   pwsh tools\win\bc250mon\build.ps1 -Out $env:BC250_ROOT\scratch\build\bc250mon

param([Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string]$ControlDll)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
New-Item -ItemType Directory -Force $Out | Out-Null
if (-not (Test-Path -LiteralPath $ControlDll -PathType Leaf)) { throw "Build bc250kmd_cli first and supply its bc250control.dll" }

# KmdProvider.cs repeats the driver's BC250_STAGE numbers. Refuse to build a monitor that names them wrongly.
if (Get-Command python -ErrorAction SilentlyContinue) {
    & python -m unittest discover -s $here 2>&1 | ForEach-Object { Write-Host "  $_" }
    if ($LASTEXITCODE -ne 0) { throw 'test_stages.py failed: the stage table no longer matches driver/kmd/bc250kmd.h' }
} else {
    Write-Warning 'python not found, skipping test_stages.py (stage table against driver/kmd/bc250kmd.h)'
}

& (Join-Path $here 'test-vulkan-inventory.ps1') -Out (Join-Path $Out 'inventory-check')
& (Join-Path $here 'test-start-confirmation.ps1') -Out (Join-Path $Out 'start-confirmation-check')
& (Join-Path $here 'test-graphics-summary.ps1') -Out (Join-Path $Out 'graphics-summary-check')
& (Join-Path $here 'test-telemetry.ps1') -Out (Join-Path $Out 'telemetry-check')
& (Join-Path $here 'test-graphics-api.ps1') -Out (Join-Path $Out 'graphics-api-check')
& (Join-Path $here 'test-lab-state.ps1') -Out (Join-Path $Out 'lab-state-check')

$refs = 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.Web.Extensions.dll' |
    ForEach-Object { "/reference:$fx\$_" }
& $csc /nologo /noconfig /nostdlib+ "/reference:$fx\mscorlib.dll" @refs /target:winexe /platform:x64 /optimize+ /warnaserror+ `
    /langversion:7.3 "/win32manifest:$here\app.manifest" "/out:$Out\bc250mon.exe" (Get-ChildItem "$here\src\*.cs").FullName
if ($LASTEXITCODE -ne 0) { throw "csc failed ($LASTEXITCODE)" }
Copy-Item -LiteralPath $ControlDll -Destination "$Out\bc250control.dll" -Force
Copy-Item -LiteralPath "$here\graphics-modules.json" -Destination "$Out\graphics-modules.json" -Force
Get-Item "$Out\bc250mon.exe" | ForEach-Object { '{0,9}  {1}' -f $_.Length, $_.Name }
