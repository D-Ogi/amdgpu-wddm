param([Parameter(Mandatory=$true)][string]$Out,
      [Parameter(Mandatory=$true)][string]$Kits)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot\..\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$env:TEMP=$Out;$env:TMP=$Out
& python -B "$repo\tools\import\extract_dml201.py" --check
if($LASTEXITCODE){throw 'DML import check failed'}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$cl="$($msvc.FullName)\bin\Hostx64\x64\cl.exe"
$sdk="$Kits\microsoft.windows.sdk.cpp\c\Include\10.0.26100.0"
$lib="$Kits\microsoft.windows.sdk.cpp.x64\c"
$argsList=@('/nologo','/TC','/W4','/WX','/O2','/MT','/fp:precise',
    "/I$($msvc.FullName)\include","/I$sdk\ucrt","/I$repo\driver\shim\dml201",
    "/Fo$Out\","/Fe$Out\dml201_test.exe","$PSScriptRoot\dml201_test.c","$repo\driver\shim\bc250_dml201.c",
    '/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$lib\ucrt\x64","/LIBPATH:$lib\um\x64")
& $cl @argsList
if($LASTEXITCODE){throw 'DML host compilation failed'}
& "$Out\dml201_test.exe"
if($LASTEXITCODE){throw 'DML host assertions failed'}
