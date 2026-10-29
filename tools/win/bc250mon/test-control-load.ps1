# ControlDll.Load (src\ControlDll.cs) on fake install folders under -Out: the installed bc250control.dll is preferred
# over the private copy beside bc250mon.exe. Each case runs in its own process, because a loaded module stays loaded.
#   nopreload  positive control: without the preload the name binds to the private copy beside the exe
#   installed  <root>\control\bc250control.dll is used, the private copy is ignored
#   tools      only <root>\tools\bc250control.dll exists: that one is used
#   private    no install folder: the private copy is used, and the row says why
#   broken     <root>\control\bc250control.dll is not a PE image: the private copy is used, and the row names the error
# No registry value and no file of this PC are read: the test replaces ControlDll.InstallRoots.
param([Parameter(Mandatory)][string]$Out, [Parameter(Mandatory)][string]$ControlDll)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not (Test-Path -LiteralPath $ControlDll -PathType Leaf)) { throw "Build bc250kmd_cli first and supply its bc250control.dll" }
New-Item -ItemType Directory -Force $Out | Out-Null
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$csc = Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$fx = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319"
$refs = 'mscorlib.dll', 'System.dll', 'System.Core.dll', 'System.Drawing.dll', 'System.Windows.Forms.dll', 'System.Web.Extensions.dll' |
    ForEach-Object { "/reference:$fx\$_" }
$bin = Join-Path $Out 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
& $csc /nologo /noconfig /nostdlib+ @refs /target:exe /main:Bc250Mon.ControlDllTest /platform:x64 /optimize+ /warnaserror+ `
    /langversion:7.3 "/out:$bin\control-load-test.exe" (Get-ChildItem "$here\src\*.cs").FullName "$here\test\ControlDllTest.cs"
if ($LASTEXITCODE -ne 0) { throw 'Control DLL load test compilation failed' }
Copy-Item -LiteralPath $ControlDll -Destination (Join-Path $bin 'bc250control.dll') -Force

function Root([string]$name, [string[]]$subs, [switch]$Broken) {
    $root = Join-Path $Out $name
    foreach ($s in $subs) {
        New-Item -ItemType Directory -Force (Join-Path $root $s) | Out-Null
        if ($Broken) { Set-Content -LiteralPath (Join-Path $root "$s\bc250control.dll") -Value 'not a PE image' }
        else { Copy-Item -LiteralPath $ControlDll -Destination (Join-Path $root "$s\bc250control.dll") -Force }
    }
    return $root
}
$cases = @(
    @('nopreload', '-'),
    @('installed', (Root 'install-both' @('control', 'tools'))),
    @('tools', (Root 'install-tools' @('tools'))),
    @('private', '-'),
    @('broken', (Root 'install-broken' @('control') -Broken))
)
$failed = 0
foreach ($c in $cases) {
    & "$bin\control-load-test.exe" $c[0] $c[1] | ForEach-Object { "  $_" }
    if ($LASTEXITCODE -ne 0) { $failed++ }
}
if ($failed) { throw "Control DLL load test: $failed case(s) failed" }
'control DLL load test: 5 cases passed'
