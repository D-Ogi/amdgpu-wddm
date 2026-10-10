param(
    [Parameter(Mandatory)][string]$Kits,
    [Parameter(Mandatory)][string]$VulkanInclude,
    [Parameter(Mandatory)][string]$Out,
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path -LiteralPath $Out).Path
$oldTemp = $env:TEMP; $oldTmp = $env:TMP; $oldInclude = $env:INCLUDE; $oldLib = $env:LIB
try {
    $env:TEMP = $Out; $env:TMP = $Out; $env:INCLUDE = ''; $env:LIB = ''
    $exe = Join-Path $Out 'cpumembw.exe'
    if (Test-Path -LiteralPath $exe) {
        $hash = (Get-FileHash -LiteralPath $exe).Hash
        Copy-Item -LiteralPath $exe -Destination (Join-Path $Out "cpumembw-$hash.exe") -Force
    }
    & $cl @('/nologo', '/W4', '/WX', '/O2', '/MT', '/EHsc', '/std:c++17', '/Brepro',
        '/DUNICODE', '/D_UNICODE', "/I$VulkanInclude", "/I$($msvc.FullName)\include",
        "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um", "/I$sdk\Include\$KitVersion\shared", "/I$sdk\Include\$KitVersion\winrt",
        "/Fo$Out\cpumembw.obj", "/Fe$exe", (Join-Path $PSScriptRoot 'cpumembw.cpp'),
        '/link', '/Brepro', "/LIBPATH:$($msvc.FullName)\lib\x64", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64", 'kernel32.lib')
    if ($LASTEXITCODE -ne 0) { throw "cl failed: $LASTEXITCODE" }
    & $exe --self-test
    if ($LASTEXITCODE -ne 0) { throw 'CPU self-test failed' }
    Get-FileHash -LiteralPath $exe
} finally { $env:TEMP = $oldTemp; $env:TMP = $oldTmp; $env:INCLUDE = $oldInclude; $env:LIB = $oldLib }
