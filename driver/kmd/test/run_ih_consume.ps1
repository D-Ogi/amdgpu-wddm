param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\ih-consume",[string]$Source='', [switch]$ExpectFailure)
$ErrorActionPreference='Stop'
$repo=Join-Path $Root 'bc250-win'
$env:TEMP=Join-Path $Root 'scratch\tmp';$env:TMP=$env:TEMP
New-Item -ItemType Directory -Force $Out | Out-Null
if(!$Source){$Source=Join-Path $repo 'driver\kmd\ih.c'}
$text=Get-Content -LiteralPath $Source -Raw
$start=$text.IndexOf('static BOOLEAN Consume(')
if($start -lt 0){$start=$text.IndexOf('static void Consume(')}
if($text -match '(?m)^<<<<<<<|^=======|^>>>>>>>'){throw 'Unresolved source conflict'}
$end=$text.IndexOf('// Caller holds GartLock',$start)
if($start -lt 0 -or $end -le $start){throw 'Consume extraction failed'}
[IO.File]::WriteAllText((Join-Path $Out 'ih_consume_actual.inc'),$text.Substring($start,$end-$start))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ih_consume_test.c') -Destination (Join-Path $Out 'test.c') -Force
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp\c';$libs=Join-Path $Root 'toolchain\nuget\microsoft.windows.sdk.cpp.x64\c'
& "$($msvc.FullName)\bin\Hostx64\x64\cl.exe" /nologo /TC /W4 /WX /wd4201 /wd4127 /O2 /MT "/I$repo\driver\kmd" "/I$($msvc.FullName)\include" "/I$sdk\Include\10.0.26100.0\ucrt" "/Fo$Out\" "/Fe$Out\ih_consume_test.exe" "$Out\test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64"
if($LASTEXITCODE -ne 0){throw 'IH consumer host build failed'}
& "$Out\ih_consume_test.exe"
if($ExpectFailure){if($LASTEXITCODE -eq 0){throw "negative control unexpectedly passed"};Write-Host "Expected negative control failed";exit 0};exit $LASTEXITCODE
