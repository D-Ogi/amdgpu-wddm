param(
    [Parameter(Mandatory=$true)][string]$Root,
    [string]$Out = "$Root\scratch\modeset\scaler-geometry",
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$cc = Join-Path $Root 'toolchain\llvm-23.1.4-win64\bin\clang-cl.exe'
$sdk = Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c'
$libs = Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$crt = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$env:TEMP = Join-Path $Root 'scratch\tmp'
$env:TMP = $env:TEMP
$env:INCLUDE = ''; $env:LIB = ''
& $cc --version
if ($LASTEXITCODE) { throw 'Explicit clang-cl unavailable' }
& python (Join-Path $repo 'tools\import\extract_scaler_geometry.py') --check
if ($LASTEXITCODE) { throw 'Scaler geometry extraction mismatch' }
Push-Location $Out
try {
    & $cc /nologo /TC /W4 /WX /O2 /c `
        "/I$($crt.FullName)\include" "/I$sdk\Include\$KitVersion\ucrt" `
        /Foscaler_geometry_production.obj "$repo\driver\shim\bc250_scaler_geometry.c"
    if ($LASTEXITCODE) { throw 'Production geometry build failed' }
    & $cc /nologo /TC /W4 /WX /O2 /MT -fuse-ld=lld /DBC250_SCALER_GEOMETRY_TEST `
        "/I$($crt.FullName)\include" "/I$sdk\Include\$KitVersion\ucrt" `
        /Fescaler_geometry_test.exe "$repo\driver\kmd\test\scaler_geometry_test.c" `
        "$repo\driver\shim\bc250_scaler_geometry.c" `
        /link "/LIBPATH:$($crt.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
    if ($LASTEXITCODE) { throw 'Scaler geometry build failed' }
    & .\scaler_geometry_test.exe
    if ($LASTEXITCODE) { throw 'Scaler geometry checks failed' }
    & python "$repo\driver\kmd\test\scaler_geometry_oracle.py" --exe (Join-Path (Get-Location) 'scaler_geometry_test.exe')
    if ($LASTEXITCODE) { throw 'Scaler geometry rational oracle failed' }
    & python "$repo\driver\kmd\test\scaler_geometry_oracle.py" --exe (Join-Path (Get-Location) 'scaler_geometry_test.exe') --chroma
    if ($LASTEXITCODE) { throw 'Chroma geometry rational oracle failed' }
} finally { Pop-Location }
