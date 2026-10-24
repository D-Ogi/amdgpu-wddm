# Builds the open ADL subset (driver/umd/adl): atiadlxx.dll for x64 or atiadlxy.dll for x86, the test builds next
# to it, the host unit test and the AGS client, then runs the gates that need no GPU.
#
#   pwsh tools\build\build-umd-adl.ps1 [-Arch x64|x86] [-Compiler clang-cl|msvc -MsvcReason <why>] [-OutputDir <dir>]
#        [-VsInstall <dir>] [-AgsSdk <dir>] [-TestService <service>]
#
# Outputs in -OutputDir (default <BC250_ROOT>\scratch\build\umd-adl-<arch>):
#   atiadlxx.dll / atiadlxy.dll   the shipped library (atiadl.def, no test hook)
#   test\atiadl-test.dll          the same sources with BC250_ADL_TEST_HOOKS (snapshot injection, live block count)
#   test\atiadl-no-modes.dll      the shipped library without ADL2_Display_Modes_Get (AGS negative control)
#   test\atiadl-test-host.dll     only with -TestService: the test build that takes the named service (for example
#                                 nvlddmkm) for ours, so that the discovery runs against a host GPU
#   adl-unit-test.exe             the export contract against the test build (tests\adl-unit-test.cpp)
#   ags-client.exe                the AGS 6.3 client of the host tests and the lab kit (tests\ags-client.cpp)
#   build.json                    compiler, flags and SHA256 of every output
# Gates: the shipped DLL exports exactly the names of atiadl.def and imports only system DLLs (static CRT); the
# image is of -Arch; the unit test passes; the discovery runs. test-umd-adl-ags.ps1 runs the AGS host tests.
#
# Compiler: clang-cl 23.1.4 from <BC250_ROOT>\toolchain by default (owner policy 2026-10-21), x86-64-v3, the x86
# calling-convention policy of common.ps1 on x86. -Compiler msvc needs -MsvcReason.
# -AgsSdk: the AGS SDK 6.3.0 headers for ags-client.exe (default <BC250_ROOT>\ref\AGS_SDK-v6.3.0, see THIRD-PARTY.md).
param([ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '',
      [string]$OutputDir, [string]$VsInstall, [string]$AgsSdk, [string]$TestService = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root = Get-Bc250Root $repo
if (-not $OutputDir) { $OutputDir = Join-Path $root "scratch\build\umd-adl-$Arch" }
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ($OutputDir.Contains(' ')) { throw "OutputDir must not contain spaces: $OutputDir" }
if (-not $AgsSdk) { $AgsSdk = Join-Path $root 'ref\AGS_SDK-v6.3.0' }
if (-not (Test-Path -LiteralPath (Join-Path $AgsSdk 'amd_ags.h'))) { throw "no amd_ags.h in -AgsSdk $AgsSdk" }
if ($TestService -and $TestService -notmatch '^[A-Za-z0-9_]+$') { throw "-TestService takes a service name: $TestService" }
$src = Join-Path $repo 'driver\umd\adl'
$name = if ($Arch -eq 'x86') { 'atiadlxy.dll' } else { 'atiadlxx.dll' }
$obj = Join-Path $OutputDir 'obj'
$testDir = Join-Path $OutputDir 'test'
# Fresh outputs each time: with the outputs of an earlier build in place, the PDB signature (and so the image bytes)
# differs from a build into an empty directory. Only the names that this script writes are removed.
foreach ($d in @($obj, $testDir)) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
if (Test-Path -LiteralPath $OutputDir) {
    Get-ChildItem -LiteralPath $OutputDir -File | Where-Object { $_.Extension -in '.dll', '.exe', '.pdb', '.lib', '.exp' } |
        Remove-Item -Force
}
foreach ($d in @($OutputDir, $obj, $testDir)) { New-Item -ItemType Directory -Force $d | Out-Null }

$saved = Save-ProcessEnvironment
try {
    $env:TEMP = $OutputDir; $env:TMP = $OutputDir
    $install = Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc = Resolve-Bc250UmdCompiler -Root $root -Compiler $(if ($Compiler -eq 'msvc') { 'msvc' } else { 'clang' }) `
        -MsvcReason $MsvcReason -Out $OutputDir -Arch $Arch
    Write-Host "Visual Studio: $install, target $env:VSCMD_ARG_TGT_ARCH"
    # Static CRT: games of any age load this library, and it must not need a Visual C++ redistributable.
    $base = @('/nologo', '/O2', '/MT', '/W4', '/WX', '/Zi', '/GS', '/std:c++17', '/DNDEBUG', '/DWIN32_LEAN_AND_MEAN',
              '/Brepro')
    if ($Compiler -eq 'clang-cl') {
        $flags = @(ConvertTo-Bc250ClangFlags $base $Arch @($repo))
        # The x86 calling-convention policy of the clang-cl recipes (common.ps1, test-x86-callconv.ps1).
        if ($Arch -eq 'x86') { $flags += '/clang:-Werror=cast-calling-convention' }
    } else {
        $flags = $base + @('/FC', "/d1trimfile:$repo", '/arch:AVX2')
    }
    $linkDll = @('/link', '/DLL', '/Brepro', '/DEBUG', '/OPT:REF', '/OPT:ICF', '/PDBALTPATH:%_PDB%',
                 'gdi32.lib', 'user32.lib', 'setupapi.lib', 'advapi32.lib')
    $sources = @("$src\adl-main.cpp", "$src\adl-discover.cpp")

    function Build-Dll([string]$Out, [string]$Def, [string]$ObjDir, [string[]]$Defines) {
        New-Item -ItemType Directory -Force $ObjDir | Out-Null
        $pdb = [IO.Path]::ChangeExtension($Out, '.pdb')
        Invoke-Bc250UmdCompiler $cc @flags @Defines "/Fo$ObjDir\" "/Fd$ObjDir\vc.pdb" @sources "/Fe$Out" @linkDll `
            "/DEF:$Def" "/PDB:$pdb"
        if ($LASTEXITCODE -ne 0) { throw "build of $Out failed" }
    }
    Build-Dll (Join-Path $OutputDir $name) "$src\atiadl.def" "$obj\shipped" @()
    Build-Dll (Join-Path $testDir 'atiadl-test.dll') "$src\tests\atiadl-test.def" "$obj\test" @('/DBC250_ADL_TEST_HOOKS')
    Build-Dll (Join-Path $testDir 'atiadl-no-modes.dll') "$src\tests\atiadl-no-modes.def" "$obj\no-modes" @()
    if ($TestService) {
        Build-Dll (Join-Path $testDir 'atiadl-test-host.dll') "$src\tests\atiadl-test.def" "$obj\host" `
            @('/DBC250_ADL_TEST_HOOKS', "/DBC250_ADL_TEST_SERVICE=$TestService")
    }
    Invoke-Bc250UmdCompiler $cc @flags "/Fo$obj\" "/Fd$obj\unit.pdb" "$src\tests\adl-unit-test.cpp" `
        "/Fe$(Join-Path $OutputDir 'adl-unit-test.exe')" /link /Brepro
    if ($LASTEXITCODE -ne 0) { throw 'adl-unit-test build failed' }
    # The AGS header is the SDK's: a system header for the warning policy. Its D3D forward declarations are left
    # out in favour of the real headers, and the D3D11 half is not needed.
    Invoke-Bc250UmdCompiler $cc @flags "/imsvc$AgsSdk" /DAGS_EXCLUDE_DIRECTX_TYPES /DAGS_EXCLUDE_DIRECTX_11 "/Fo$obj\" `
        "/Fd$obj\client.pdb" "$src\tests\ags-client.cpp" "/Fe$(Join-Path $OutputDir 'ags-client.exe')" /link /Brepro `
        d3d12.lib dxgi.lib psapi.lib
    if ($LASTEXITCODE -ne 0) { throw 'ags-client build failed' }

    Push-Location $OutputDir
    try {
        # Gate: the image is of -Arch.
        $machine = (& dumpbin.exe /nologo /headers $name | Select-String 'machine \(') -join ' '
        $want = if ($Arch -eq 'x86') { '14C machine (x86)' } else { '8664 machine (x64)' }
        if (-not $machine.Contains($want)) { throw "$name is not $Arch ($machine)" }
        # Gate: exactly the exports of atiadl.def, so no test hook ships.
        $wantExports = @(Get-Content -LiteralPath "$src\atiadl.def" | Where-Object { $_ -match '^\s+(\w+)\s*$' } |
            ForEach-Object { $Matches[1] } | Sort-Object)
        $gotExports = @(& dumpbin.exe /nologo /exports $name | ForEach-Object {
            if ($_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\w+)') { $Matches[1] } } | Sort-Object)
        $diff = @(Compare-Object $wantExports $gotExports)
        if ($diff.Count -or $gotExports.Count -ne 31) {
            throw "export gate: $name differs from atiadl.def: $(($diff | ForEach-Object { "$($_.SideIndicator)$($_.InputObject)" }) -join ' ')"
        }
        Write-Host "PASS export gate: $($gotExports.Count) exports, as atiadl.def"
        # Gate: system DLLs only (static CRT).
        $deps = @(& dumpbin.exe /nologo /dependents $name | Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } |
            ForEach-Object { $Matches[1].ToUpperInvariant() })
        $allowed = @('KERNEL32.DLL', 'USER32.DLL', 'GDI32.DLL', 'SETUPAPI.DLL', 'ADVAPI32.DLL')
        $foreign = @($deps | Where-Object { $allowed -notcontains $_ })
        if ($foreign.Count) { throw "dependency gate: $name imports $($foreign -join ', ')" }
        Write-Host "PASS dependency gate: $($deps -join ', ')"
        # Gate: the export contract, and the discovery on this machine.
        & "$OutputDir\adl-unit-test.exe" "$testDir\atiadl-test.dll" unit 2>&1 | Tee-Object -FilePath "$OutputDir\unit.txt"
        if ($LASTEXITCODE -ne 0) { throw 'adl-unit-test failed' }
        & "$OutputDir\adl-unit-test.exe" "$testDir\atiadl-test.dll" discover 2>&1 |
            Tee-Object -FilePath "$OutputDir\discover.txt"
        if ($LASTEXITCODE -ne 0) { throw 'discovery run failed' }
        if ($TestService) {
            & "$OutputDir\adl-unit-test.exe" "$testDir\atiadl-test-host.dll" discover 2>&1 |
                Tee-Object -FilePath "$OutputDir\discover-host.txt"
            if ($LASTEXITCODE -ne 0) { throw 'host discovery run failed' }
        }
    } finally { Pop-Location }

    $outputs = @(Get-ChildItem -LiteralPath $OutputDir -Recurse -File -Include '*.dll', '*.exe' |
        Where-Object { $_.FullName -notlike "$obj\*" } | Sort-Object FullName)
    [ordered]@{
        arch = $Arch
        compiler = (Get-Content -LiteralPath (Join-Path $OutputDir 'compiler.json') -Raw | ConvertFrom-Json)
        flags = $flags
        windows_sdk_version = $env:WindowsSDKVersion
        msvc_tools_version = $env:VCToolsVersion
        test_service = $TestService
        ags_sdk = $AgsSdk
        outputs = @($outputs | ForEach-Object {
            [ordered]@{ path = $_.FullName.Substring($OutputDir.Length + 1); sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash } })
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDir 'build.json') -Encoding utf8
} finally { Restore-ProcessEnvironment $saved }
'{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $OutputDir $name)).Hash, $name
