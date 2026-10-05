# Builds gpu-timeline.exe (user mode only; no driver) with the installed Visual Studio compiler and the SDK NuGet
# packages under -Kits, then runs its selftest and the analyzer's tests. The artifacts go to -Out, outside this
# repository (BC250_ROOT is the workspace root, by default the parent directory of the repository).
#
#   pwsh tools\win\gpu-timeline\build.ps1
#   pwsh tools\win\gpu-timeline\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Out <BC250_ROOT>\scratch\build\gpu-timeline
#
# Inputs from the repository: tools\win\bc250rd\bc250rd_ioctl.h (the IOCTL codes) and the vendored
# third_party\linux-amdgpu\gc_10_1_0_sh_mask.h (field masks); gtl_regs.h is regenerated through tools\regcalc first.
param(
    [string]$Root = '',
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
if (-not $Root) { $Root = if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Split-Path -Parent $repo) } }
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\gpu-timeline' }
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'
$out = $Out
New-Item -ItemType Directory -Force $out | Out-Null

& python (Join-Path $here 'gen_regs.py') --repo $repo --out (Join-Path $here 'gtl_regs.h')
if ($LASTEXITCODE -ne 0) { throw 'gen_regs.py failed' }

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$env:INCLUDE = ''; $env:LIB = ''
# /Brepro: no time stamp in the image, so the same sources and toolchain give the same SHA256.
& $cl /nologo /W4 /WX /O2 /MT /Brepro /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN `
    "/I$(Join-Path $msvc.FullName 'include')" "/I$sdk\Include\$KitVersion\ucrt" "/I$sdk\Include\$KitVersion\um" `
    "/I$sdk\Include\$KitVersion\shared" "/I$(Join-Path $repo 'tools\win\bc250rd')" `
    "/I$(Join-Path $repo 'third_party\linux-amdgpu')" "/I$here" `
    "/Fo$out\gpu-timeline.obj" "/Fe$out\gpu-timeline.exe" (Join-Path $here 'gpu-timeline.c') `
    /link /Brepro "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')" "/LIBPATH:$sdkLib\ucrt\x64" "/LIBPATH:$sdkLib\um\x64"
if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

# Provenance of the exact artifact: hashes of what went in and what came out, and the repository revision.
$inputs = @(
    (Join-Path $here 'gpu-timeline.c'), (Join-Path $here 'gtl_regs.h'), (Join-Path $here 'gen_regs.py'),
    (Join-Path $here 'analyze.py'), (Join-Path $here 'test_analyze.py'), (Join-Path $here 'gtl-host.py'),
    (Join-Path $here 'lab\gtl-run.ps1'), (Join-Path $here 'lab\test-invoke-gtl.ps1'),
    (Join-Path $repo 'tools\win\bc250rd\bc250rd_ioctl.h'), (Join-Path $repo 'tools\win\bc250rd\driver\allowlist.h'),
    (Join-Path $repo 'third_party\linux-amdgpu\gc_10_1_0_sh_mask.h'), (Join-Path $repo 'tools\regcalc\regcalc.py'),
    (Join-Path $out 'gpu-timeline.exe'))
$head = (& git -C $repo rev-parse HEAD).Trim()
$lines = @("bc250-win HEAD $head", "msvc $($msvc.Name)", "sdk $KitVersion")
foreach ($f in $inputs) { $lines += '{0}  {1}' -f (Get-FileHash -LiteralPath $f -Algorithm SHA256).Hash, $f }
$lines | Set-Content -LiteralPath (Join-Path $out 'build-info.txt') -Encoding ascii
$lines | ForEach-Object { Write-Host $_ }

# Self-checks on this PC: no device is opened by either.
& (Join-Path $out 'gpu-timeline.exe') selftest --out (Join-Path $out 'selftest.gtl')
if ($LASTEXITCODE -ne 0) { throw 'selftest failed' }
$env:BC250_GTL_BUILD = $out   # so that the analyzer's selftest case finds the exe outside the repository
& python -m unittest discover -s $here -p 'test_*.py' -v
if ($LASTEXITCODE -ne 0) { throw 'analyzer tests failed' }
# The lab wrapper's process handling under Windows PowerShell 5.1, the lab's shell.
& "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass `
    -File (Join-Path $here 'lab\test-invoke-gtl.ps1')
if ($LASTEXITCODE -ne 0) { throw 'gtl-run.ps1 Invoke-Gtl check failed' }
