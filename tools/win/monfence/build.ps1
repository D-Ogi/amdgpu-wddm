# Builds monfence.exe and its host test without a WDK or SDK installation: headers and import libraries come from the
# SDK NuGet packages under -Kits, the compiler from the installed Visual Studio (as tools\win\kmtprobe\build.ps1).
# Then runs everything that can run on the development PC: the host test (packets against nvd.h / navi10_enum.h /
# the KMD's fence packet and Linux's ring words, blobs through the KMD's own reader), the client's --selftest and
# --help, a parse of run-lab.ps1, and the constant checks below. Prints the SHA256 of what it built.
#
#   pwsh bc250-win\tools\win\monfence\build.ps1
#   pwsh bc250-win\tools\win\monfence\build.ps1 -Kits P:\bc-250\toolchain\nuget -Out P:\bc-250\scratch\build\monfence

param(
    [string]$Root = $(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),
    [string]$Kits = '',
    [string]$Out = '',
    [string]$KitVersion = '10.0.26100.0'
)

$ErrorActionPreference = 'Stop'
if (-not $Kits) { $Kits = Join-Path $Root 'toolchain\nuget' }
if (-not $Out) { $Out = Join-Path $Root 'scratch\build\monfence' }
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $here '..\..\..')).Path
$sdk = Join-Path $Kits 'microsoft.windows.sdk.cpp\c'
$sdkLib = Join-Path $Kits 'microsoft.windows.sdk.cpp.x64\c'

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$cl = Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
New-Item -ItemType Directory -Force $Out | Out-Null
# Compiler temporaries stay off drive C: of the development PC.
$env:TEMP = Join-Path $Root 'scratch\tmp'; $env:TMP = $env:TEMP
New-Item -ItemType Directory -Force $env:TEMP | Out-Null

# Constants restated outside their source, compared here (repo rules 1 and 2: a copy that drifts is a build failure).
function Assert-Same($SourcePath, $SourcePattern, $ToolPath, $ToolPattern, $Name) {
    foreach ($p in @($SourcePath, $ToolPath)) { if (-not (Test-Path $p)) { throw "$p not found ($Name check)" } }
    $s = Select-String -Path $SourcePath -Pattern $SourcePattern | Select-Object -First 1
    $t = Select-String -Path $ToolPath -Pattern $ToolPattern | Select-Object -First 1
    if ($null -eq $s) { throw "$Name not found in $SourcePath - renamed?" }
    if ($null -eq $t) { throw "$Name not found in $ToolPath" }
    $a = $s.Matches[0].Groups[1].Value -replace 'u$', ''
    $b = $t.Matches[0].Groups[1].Value -replace 'u$', ''
    $va = if ($a -match '^0x') { [Convert]::ToUInt64($a.Substring(2), 16) } else { [UInt64]$a }
    $vb = if ($b -match '^0x') { [Convert]::ToUInt64($b.Substring(2), 16) } else { [UInt64]$b }
    if ($va -ne $vb) { throw "$Name differs: $(Split-Path -Leaf $SourcePath) $a, $(Split-Path -Leaf $ToolPath) $b" }
    Write-Host "  $Name $b matches $(Split-Path -Leaf $SourcePath)"
}
$packets = Join-Path $here 'monfence_packets.h'
$runner = Join-Path $here 'run-lab.ps1'
$clientIds = Join-Path $repo 'third_party\linux-amdgpu\soc15_ih_clientid.h'
Assert-Same (Join-Path $repo 'driver\shim\bc250_gfx.c') '#define\s+BC250_CP_NOP\s+(0x[0-9A-Fa-f]+u?)' $packets '#define\s+MF_CP_NOP\s+(0x[0-9A-Fa-f]+u?)' 'CP NOP pad'
Assert-Same $clientIds 'SOC15_IH_CLIENTID_UTCL2\s*=\s*(0x[0-9A-Fa-f]+)' $runner '^\$UtcL2Client\s*=\s*(\d+)' 'UTCL2 client id (run-lab.ps1)'
Assert-Same $clientIds 'SOC15_IH_CLIENTID_VMC\s*=\s*(0x[0-9A-Fa-f]+)' $runner '^\$VmcClient\s*=\s*(\d+)' 'VMC client id (run-lab.ps1)'

# run-lab.ps1 runs under Windows PowerShell 5.1 on the lab: it must at least parse.
$tokens = $null; $errors = $null
[void][System.Management.Automation.Language.Parser]::ParseFile($runner, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { $errors | ForEach-Object { Write-Host "  $_" }; throw 'run-lab.ps1 does not parse' }
Write-Host '  run-lab.ps1 parses'

$common = @('/nologo', '/W4', '/WX', '/O2', '/MT', '/D_CRT_SECURE_NO_WARNINGS',
    "/I$(Join-Path $msvc.FullName 'include')", "/I$sdk\Include\$KitVersion\ucrt", "/I$sdk\Include\$KitVersion\um",
    "/I$sdk\Include\$KitVersion\shared", "/I$(Join-Path $repo 'driver\contract\third_party')",
    "/I$(Join-Path $repo 'driver\contract\uapi-shim')")
$link = @('/link', "/LIBPATH:$(Join-Path $msvc.FullName 'lib\x64')", "/LIBPATH:$sdkLib\ucrt\x64", "/LIBPATH:$sdkLib\um\x64")
function Invoke-Cl([string[]]$Arguments, [string]$What) {
    $env:INCLUDE = ''; $env:LIB = ''
    & $cl @Arguments | ForEach-Object { if ($_ -notmatch '^\s*$|^Microsoft|^Copyright|^\S+\.c$') { Write-Host "  $_" } }
    if ($LASTEXITCODE -ne 0) { throw "cl failed for $What ($LASTEXITCODE)" }
}

# Host test: the client's headers plus the KMD's own blob reader.
Invoke-Cl ($common + @("/Fo$Out\", "/Fe$Out\monfence_host_test.exe", (Join-Path $here 'monfence_host_test.c'),
    (Join-Path $repo 'driver\kmd\umd_blob.c')) + $link) 'monfence_host_test'
& "$Out\monfence_host_test.exe"
if ($LASTEXITCODE -ne 0) { throw "monfence_host_test failed ($LASTEXITCODE checks)" }

# The client.
Invoke-Cl ($common + @("/Fo$Out\monfence.obj", "/Fe$Out\monfence.exe", (Join-Path $here 'monfence.c')) + $link + @('gdi32.lib')) 'monfence'
& "$Out\monfence.exe" --selftest
if ($LASTEXITCODE -ne 0) { throw "monfence --selftest failed ($LASTEXITCODE)" }
& "$Out\monfence.exe" --help | Select-Object -First 1 | ForEach-Object { Write-Host "  $_" }
if ($LASTEXITCODE -ne 0) { throw "monfence --help failed ($LASTEXITCODE)" }

foreach ($f in @('monfence.exe', 'monfence_host_test.exe')) {
    $item = Get-Item (Join-Path $Out $f)
    $hash = (Get-FileHash -Algorithm SHA256 $item.FullName).Hash
    '{0,9}  {1}  sha256 {2}' -f $item.Length, $item.Name, $hash
}
