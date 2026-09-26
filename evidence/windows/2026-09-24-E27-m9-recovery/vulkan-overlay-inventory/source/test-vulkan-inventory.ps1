param([Parameter(Mandatory)][string]$Out)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $Out | Out-Null
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Web.Extensions.dll' | ForEach-Object { "/reference:$fx\$_" }
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /platform:x64 /optimize+ /warnaserror+ /langversion:7.3 "/out:$Out\vulkan-inventory-test.exe" "$here\src\State.cs" "$here\src\VulkanInventoryProvider.cs" "$here\src\OverlayLayout.cs" "$here\test\VulkanInventoryTest.cs"
if ($LASTEXITCODE -ne 0) { throw 'Inventory check compilation failed' }
& "$Out\vulkan-inventory-test.exe" "$here\test\fixtures" "$Out\data"
if ($LASTEXITCODE -ne 0) { throw 'Inventory check failed' }
