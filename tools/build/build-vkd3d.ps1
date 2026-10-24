#Requires -Version 7.0
<#
.SYNOPSIS
Configures and builds vkd3d-proton with a recorded meson option set (vkd3d-configs.json).

.DESCRIPTION
-Config picks the option set from vkd3d-configs.json next to this script (the only copy of those options):

  per-app     upstream vkd3d-proton as application-local d3d12.dll and d3d12core.dll (M12)
  ddi-engine  amdgpu_wddm_vkd3d.dll, the engine behind the proposed system D3D12 DDI UMD (G4), and its offline
              test; needs a checkout of the amdgpu-wddm/ddi-engine vkd3d-proton branch (enable_ddi_engine does
              not exist upstream)
  ddi-engine-lto
              the ddi-engine options followed by the optimisation options of the released engine (LTO, AVX2:
              /arch:AVX2 with msvc, x86-64-v3 with clang-cl, WARN and FIXME compiled out in release, a linker
              map). Lab keeps WARN and FIXME.

-Arch x86 builds the 32-bit (WoW64) engine with vcvarsamd64_x86.bat (common.ps1), as build-dxvk.ps1 does.

The build is a native Windows build in the Visual Studio developer environment, like the M12 per-application
packages: vcvars64.bat, meson from PYTHONPATH, ninja and glslang on PATH. vkd3d-proton also needs the IDL compiler
widl (MSYS2 mingw64 ships one). A native file in the build directory names it to meson ([binaries] widl), so the
order of PATH does not select it. Before configure the script gives widl a probe interface with a method that
returns a structure: the C vtable must take the explicit return pointer (WIDL_EXPLICIT_AGGREGATE_RETURNS, which
vkd3d_d3d12.idl defines and the vkd3d-proton source expects). widl 1.6 gives the implicit form, and the script stops.
After configure, the "Program widl found" line of meson must name the same file, and recipe.json records it.
After configure the script also compares the new log's "Build Options:" line with the option set and
stops on any difference. The set the script passes is the configured set plus the reproducible-build switches
of Add-ReproducibleMesonOptions (common.ps1): /Brepro, /FC, /d1trimfile and /PDBALTPATH:%_PDB% for msvc. The
switches go at the end of the c_args, cpp_args and cpp_link_args that ddi-engine-lto already has.

-Compiler has no default: no build falls back to cl.exe without a word. msvc builds with cl, link and lib of the
Visual Studio environment, as every engine up to tester.32 was built. clang-cl builds with clang-cl, lld-link and
llvm-lib from -ClangToolchain (default LLVM 23.1.4), in the same Visual Studio environment for the headers, the
libraries and rc.exe, and adds the arguments of its 'compilers' entry in vkd3d-configs.json. A config can hold a
'compiler_options' map: the options of each compiler, after the config's own 'options' (ddi-engine-lto: LTO mode,
instruction set and linker map, which the two compilers spell differently). The script prints the path and version
of the compiler and the linker, and stops when one of them is missing. -Arch x86 with clang-cl names the
i686-pc-windows-msvc target in the compiler command, so that meson detects an x86 host, and takes the clang-cl form of
the x86 calling-convention policy from Add-ReproducibleMesonOptions (-Werror=ignored-attributes and
-Werror=cast-calling-convention). After configure, every compile command must carry the target and the policy and no
switch that turns warnings off or makes a directory a system directory; the script stops when one does not.

The engine links the static C runtime (-Db_vscrt=mt): after the build, each target that imports a C or C++ runtime
DLL (msvcp*, vcruntime*, concrt*, ucrtbase, api-ms-win-crt-*) stops the script, as in build-mesa.ps1.

The source tree must be a vkd3d-proton checkout with its submodules initialized (git submodule update --init
--recursive); this script patches nothing and fetches nothing.

.EXAMPLE
pwsh tools\build\build-vkd3d.ps1 -Config ddi-engine -Compiler msvc -Source P:\BC-250\scratch\m15\vkd3d -Build P:\BC-250\scratch\m15\vkd3d-ddi-engine -Widl C:\msys64\mingw64\bin\widl.exe

.EXAMPLE
pwsh tools\build\build-vkd3d.ps1 -Config ddi-engine-lto -Compiler clang-cl -Source <vkd3d-proton fork tree> -Build <dir>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('per-app', 'ddi-engine', 'ddi-engine-lto')][string]$Config,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Build,
    [switch]$ConfigureOnly,
    # Re-run meson on an already configured build directory (meson setup --reconfigure).
    [switch]$Reconfigure,
    # Parallel jobs for ninja; 0 leaves the choice to ninja.
    [int]$Jobs = 0,
    # Directory holding meson (and packaging) for PYTHONPATH (default <BC250_ROOT>\scratch\py).
    [string]$PythonPath,
    # python.exe that runs meson; default: python on PATH.
    [string]$Python,
    # Directory holding glslangValidator.exe (default <BC250_ROOT>\scratch\glslang\bin).
    [string]$Glslang,
    # widl.exe; default: the first widl.exe on PATH. meson takes this file from a native file, not from PATH.
    [string]$Widl,
    # ninja.exe; its directory goes in front of PATH. Default: whatever PATH gives.
    [string]$Ninja,
    [string]$VsInstall,
    # TEMP and TMP for the build (default <BC250_ROOT>\scratch\tmp).
    [string]$Temp,
    # Target architecture: x86 builds the 32-bit (WoW64) DLL with vcvarsamd64_x86.bat (common.ps1).
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
    # BD-166: the version resource the shipped artifacts of this build carry, written by this build as its last
    # step. -Version is the package version, which the release passes to every component build it runs, and
    # -Flavour is that release's flavour. An artifact no release ships is not stamped, and without -Version
    # nothing is: a host build is not a release, and the release reads every resource back before it ships.
    [string]$Version = '',
    [ValidateSet('release', 'lab')][string]$Flavour = 'release',
    # BD-167: the lab flavour (-Damdgpu-wddm-lab-flavour=true). Without it the fork builds its release flavour,
    # which has no lookup of the override variables; the fork's own host test describes them.
    [switch]$Lab,
    # The C and C++ compiler, which every caller names: msvc (cl, link, lib) or clang-cl (clang-cl, lld-link,
    # llvm-lib of -ClangToolchain).
    [Parameter(Mandatory)][ValidateSet('msvc', 'clang-cl')][string]$Compiler,
    # -Compiler clang-cl: the LLVM Windows toolchain whose bin\ holds clang-cl.exe, lld-link.exe and llvm-lib.exe
    # (default <BC250_ROOT>\toolchain\llvm-23.1.4-win64).
    [string]$ClangToolchain
)
# One flavour, two names: -Lab is BD-167's and -Flavour is BD-166's, and the release passes the one it knows.
# Without this line a caller that passes one knob alone ships a release-flavour binary stamped lab, or the reverse.
if ($Lab) { $Flavour = 'lab' } elseif ($Flavour -eq 'lab') { $Lab = $true }

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$configsPath = Join-Path $PSScriptRoot 'vkd3d-configs.json'
$configsFile = Get-Content -LiteralPath $configsPath -Raw | ConvertFrom-Json
$recipeConfig = $configsFile.configs.$Config
$options = @($recipeConfig.options)
# The options of this compiler, after the config's own: the two compilers spell the LTO mode, the instruction set
# and the linker map differently. A config with the map and no entry for this compiler is an error: it would build
# something else than its name says. The key is looked up before it is read (Set-StrictMode, as release_options).
if ($recipeConfig.PSObject.Properties['compiler_options']) {
  if (-not $recipeConfig.compiler_options.PSObject.Properties[$Compiler]) {
    throw "-Config $Config has no compiler_options for -Compiler $Compiler (it has $(@($recipeConfig.compiler_options.PSObject.Properties.Name) -join ', '))"
  }
  $options += @($recipeConfig.compiler_options.$Compiler | Where-Object { $_ })
}
if ($Lab) {
  # BD-180: VKD3D_DEBUG/VKD3D_SHADER_DEBUG cannot enable warnings that the compiler removed.
  # Keep the release recipe intact; remove only this complete c_args token for the lab flavour.
  $options = @($options | ForEach-Object {
    if ($_.StartsWith('-Dc_args=')) {
      '-Dc_args=' + (($_.Substring('-Dc_args='.Length) -split '\s+' |
        Where-Object { $_ -and $_ -ne '-DVKD3D_NO_DEBUG_MESSAGES' }) -join ' ')
    } else { $_ }
  })
  $options += '-Damdgpu-wddm-lab-flavour=true'
}
$targets = @($recipeConfig.targets)

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Get-Bc250Root $repo
if (-not $PythonPath) { $PythonPath = Join-Path $root 'scratch\py' }
if (-not $Glslang) { $Glslang = Join-Path $root 'scratch\glslang\bin' }
if (-not $Temp) { $Temp = Join-Path $root 'scratch\tmp' }
$Source = [IO.Path]::GetFullPath($Source)
$Build = [IO.Path]::GetFullPath($Build)
# BD-167: the release flavour of a fork that declares the flavour also takes the config's release_options, as in
# build-mesa.ps1 and build-dxvk.ps1. No config here has one: ddi-engine-lto defines NDEBUG in both flavours.
$forkFlavour = Get-AmdgpuWddmFlavour $Source -Lab:$Lab
# A config without a release_options key is normal (nothing extra in its release flavour), but
# common.ps1 sets Set-StrictMode -Version 3.0, under which reading an absent property is an error,
# not $null - so the key is looked up before it is read.
if ($forkFlavour -eq 'release' -and $recipeConfig.PSObject.Properties['release_options']) {
  $options += @($recipeConfig.release_options | Where-Object { $_ })
}
# The compiler's own arguments: additions that leave the recorded set as it is.
$options = Add-MesonVariants $options $configsFile $Compiler
# clang-cl for x86 is the i686 target, named in the compiler command below (CC, CXX), so that meson detects an x86
# host machine; common.ps1 then adds the clang-cl form of the x86 calling-convention policy.
$clangTarget = if ($Compiler -eq 'clang-cl' -and $Arch -eq 'x86') { 'i686-pc-windows-msvc' } else { '' }
$options = Add-ReproducibleMesonOptions $options $Source $Build -Arch $Arch -Compiler $Compiler -ClangTarget $clangTarget
$clangTools = $null
if ($Compiler -eq 'clang-cl') {
    if (-not $ClangToolchain) { $ClangToolchain = Join-Path $root 'toolchain\llvm-23.1.4-win64' }
    $ClangToolchain = [IO.Path]::GetFullPath($ClangToolchain)
    if ($ClangToolchain -match '\s') { throw "-ClangToolchain $ClangToolchain contains white space: meson splits CC at white space" }
    $clangTools = [ordered]@{}
    foreach ($t in 'clang-cl', 'lld-link', 'llvm-lib') {
        $exe = Join-Path $ClangToolchain "bin\$t.exe"
        if (-not (Test-Path -LiteralPath $exe)) { throw "$exe does not exist: -ClangToolchain takes an LLVM Windows toolchain directory" }
        $clangTools[$t] = $exe
    }
}
# A static C runtime build must import no C or C++ runtime DLL (docs/build.md, radv-system).
$staticCrt = $options -contains '-Db_vscrt=mt'

if (-not (Test-Path -LiteralPath (Join-Path $Source 'libs\vkd3d\vkd3d_private.h'))) { throw "$Source is not a vkd3d-proton source tree" }
# An uninitialized submodule is an empty directory; meson would fail much later with a less useful message.
foreach ($sub in 'khronos\Vulkan-Headers\include', 'khronos\SPIRV-Headers\include', 'subprojects\dxil-spirv\meson.build',
        'subprojects\dxil-spirv\third_party\SPIRV-Cross\CMakeLists.txt') {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $sub))) {
        throw "$Source\$sub is missing: run git -C $Source submodule update --init --recursive"
    }
}
if ($Config -like 'ddi-engine*' -and -not (Test-Path -LiteralPath (Join-Path $Source 'libs\ddi\bc250_vkd3d_engine.h'))) {
    throw "$Source has no libs\ddi: -Config ddi-engine needs the amdgpu-wddm/ddi-engine branch"
}
if (-not (Test-Path -LiteralPath (Join-Path $Glslang 'glslangValidator.exe'))) { throw "glslangValidator.exe not found in ${Glslang}: pass -Glslang" }
$widlExe = Resolve-Tool $Widl 'widl'
if (-not $widlExe) { throw 'widl not found: put it on PATH or pass -Widl (MSYS2 mingw64 ships it)' }
$widlExe = [IO.Path]::GetFullPath($widlExe)
if ($widlExe -match "'") { throw "-Widl $widlExe contains a quote: the meson native file cannot name it" }

# vkd3d_d3d12.idl defines WIDL_EXPLICIT_AGGREGATE_RETURNS, and the source implements each COM method that returns a
# structure with the explicit form: a return pointer after This, returned again. A widl that does not know the macro
# (1.6) writes the vtable with the structure as the return value. MSVC warns on that mismatch, and calls through
# the vtable from C then pass no return pointer. clang 23 stops with -Wincompatible-function-pointer-types. The
# probe is one interface with one such method; the C vtable that widl writes for it must take the return pointer.
function Test-WidlAggregateReturns([string]$WidlExe, [string]$Dir) {
    New-Item -ItemType Directory -Force $Dir | Out-Null
    $idl = Join-Path $Dir 'probe.idl'
    $header = Join-Path $Dir 'probe.h'
    Remove-Item -LiteralPath $header -ErrorAction SilentlyContinue
    Set-Content -LiteralPath $idl -Encoding ascii -Value @(
        'typedef struct probe_luid { unsigned long low; long high; } probe_luid;',
        '[object, local, uuid(5c3e3a2b-6f1d-4c8e-9a51-2b7d0e4f8a10)]',
        'interface IProbe', '{', '    probe_luid GetLuid();', '}')
    $r = Invoke-Capture $WidlExe @('-h', '-o', $header, $idl)
    if ($r.Code -ne 0 -or -not (Test-Path -LiteralPath $header)) { throw "widl probe: $WidlExe exit $($r.Code)`n$($r.Err)" }
    $text = Get-Content -LiteralPath $header -Raw
    $explicit = $text -match 'probe_luid\s*\*\s*\(STDMETHODCALLTYPE \*GetLuid\)\(\s*IProbe \*This,\s*probe_luid \*__ret\)'
    $implicit = $text -match 'probe_luid\s*\(STDMETHODCALLTYPE \*GetLuid\)\(\s*IProbe \*This\)'
    return [ordered]@{ probe = $Dir; explicit_aggregate_returns = $explicit; implicit_vtable = $implicit }
}
$configured = Test-Path -LiteralPath (Join-Path $Build 'meson-private\coredata.dat')
if ($configured -and -not $Reconfigure) { throw "$Build is already configured: pass -Reconfigure, or use a new build directory" }

function Get-MesonBuildOptions([string]$BuildDir) {
    $log = Join-Path $BuildDir 'meson-logs\meson-log.txt'
    $line = Get-Content -LiteralPath $log -TotalCount 40 | Where-Object { $_.StartsWith('Build Options: ') } | Select-Object -First 1
    if (-not $line) { throw "no 'Build Options:' line in $log" }
    return @([regex]::Matches($line.Substring('Build Options: '.Length), "'[^']*'|\S+") | ForEach-Object { $_.Value.Trim("'") })
}

# Submodule commits are part of the source identity: the DXIL and DXBC translators live in dxil-spirv.
function Get-SubmoduleStatus([string]$Tree) {
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if (-not $git) { return $null }
    $r = Invoke-Capture $git.Source @('-C', $Tree, '--no-optional-locks', 'submodule', 'status', '--recursive')
    if ($r.Code -ne 0) { return $null }
    return @($r.Out -split "`r?`n" | Where-Object { $_.Trim() } | ForEach-Object { $_.Trim() })
}

$recipePath = Join-Path $Build 'recipe.json'
$savedEnv = Save-ProcessEnvironment
try {
    $vs = Import-VsDevEnvironment $VsInstall $Temp $Arch
    New-Item -ItemType Directory -Force $Temp, $Build | Out-Null
    $env:TEMP = $Temp
    $env:TMP = $Temp
    $pathFront = @($Glslang)
    if ($Ninja) { $pathFront += (Split-Path -Parent (Resolve-Tool $Ninja 'ninja')) }
    Add-PathFront $pathFront
    # find_program('widl') of vkd3d-proton reads [binaries] of the native file before PATH. Another widl on PATH
    # (an old MinGW) then cannot replace the one that the probe checked and recipe.json records.
    $widlProbe = Test-WidlAggregateReturns $widlExe (Join-Path $Build 'widl-probe')
    if (-not $widlProbe.explicit_aggregate_returns) {
        throw ("widl probe: $widlExe ($(Get-ToolVersion $widlExe @('-V'))) writes no explicit aggregate return " +
            "(WIDL_EXPLICIT_AGGREGATE_RETURNS) in the C vtable of $($widlProbe.probe)\probe.h. vkd3d-proton needs it: " +
            'pass -Widl with a newer widl (MSYS2 mingw64 ships one)')
    }
    $nativeFile = Join-Path $Build 'widl-native.ini'
    Set-Content -LiteralPath $nativeFile -Encoding ascii -Value @('[binaries]', "widl = '$($widlExe -replace '\\', '/')'")
    $env:PYTHONPATH = $PythonPath
    # meson takes the compilers, their linker and the static linker from these variables. Without them it would take
    # the first compiler it finds (gcc from a MinGW directory on PATH, the widl directory included), and a missing
    # cl.exe would give a build with another compiler. -Compiler msvc names the tools of the Visual Studio environment
    # as meson's own search finds them. -Compiler clang-cl names the tools of -ClangToolchain, inside the same Visual
    # Studio environment, whose INCLUDE and LIB clang-cl and lld-link read. llvm-lib and not lib.exe, because the
    # objects of a ThinLTO build are LLVM bitcode. For x86 the compiler command names the i686 target: meson takes the
    # host machine from the target that the compiler reports, and the x86 Visual Studio environment does not change
    # the target of clang-cl. CL and _CL_ go: clang-cl reads them, and the x86 environment puts the MSVC policy in
    # _CL_, which clang-cl ignores at compile and appends after /link at a link, where lld-link reads each switch as a
    # file. The clang-cl policy is in the option set (Add-ReproducibleMesonOptions).
    if ($clangTools) {
        $env:CC = (@($clangTools['clang-cl']) + @($clangTarget | Where-Object { $_ } | ForEach-Object { "--target=$_" })) -join ' '
        $env:CXX = $env:CC
        $env:CL = $null
        $env:_CL_ = $null
        $env:CC_LD = $clangTools['lld-link']
        $env:CXX_LD = $clangTools['lld-link']
        $env:AR = $clangTools['llvm-lib']
    } else {
        foreach ($t in 'cl', 'link', 'lib') {
            if (-not (Get-Command "$t.exe" -CommandType Application -ErrorAction SilentlyContinue)) {
                throw "-Compiler msvc: $t.exe is not on PATH after the Visual Studio environment ($vs)"
            }
        }
        $env:CC = 'cl'
        $env:CXX = 'cl'
        $env:CC_LD = 'link'
        $env:CXX_LD = 'link'
        $env:AR = 'lib'
    }

    $pythonExe = Resolve-Tool $Python 'python'
    if (-not $pythonExe) { throw 'python not found: put it on PATH or pass -Python' }
    $probe = Invoke-Capture $pythonExe @('-c', 'import mesonbuild.coredata as c; print(c.version)')
    if ($probe.Code -ne 0) {
        throw ("$pythonExe cannot import meson from PYTHONPATH=$PythonPath. " +
            "Install it there: python -m pip install --target $PythonPath meson==1.12.0`n$($probe.Err)")
    }
    $mesonVersion = $probe.Out.Trim()
    $ninjaExe = Resolve-Tool '' 'ninja'
    if (-not $ninjaExe) { throw 'ninja not found: put it on PATH or pass -Ninja' }

    $mesonArgs = @('-m', 'mesonbuild.mesonmain', 'setup')
    if ($configured) { $mesonArgs += '--reconfigure' }
    $mesonArgs += @($Build, $Source) + $options + @("--native-file=$nativeFile")

    $glslangExe = Join-Path $Glslang 'glslangValidator.exe'
    $recipe = [ordered]@{
        recipe          = 'vkd3d'
        schema          = 1
        config          = $Config
        description     = $recipeConfig.description
        utc             = [DateTime]::UtcNow.ToString('o')
        status          = 'configuring'
        recipe_identity = Get-RecipeIdentity $repo @($PSCommandPath, $configsPath, (Join-Path $PSScriptRoot 'common.ps1'))
        bc250_root      = $root
        source          = Get-SourceIdentity $Source
        submodules      = Get-SubmoduleStatus $Source
        build_dir       = $Build
        meson_options   = $options
        compiler        = $Compiler
        flavour         = $forkFlavour
        ninja_targets   = $targets
        environment     = [ordered]@{
            vs_install          = $vs
            arch                = $Arch
            vc_tools_version    = $env:VCToolsVersion
            windows_sdk_version = if ($env:WindowsSDKVersion) { $env:WindowsSDKVersion.TrimEnd('\') } else { $null }
            path_prepended      = $pathFront
            native_file         = $nativeFile
            pythonpath          = $PythonPath
            cc                  = $env:CC
            cxx                 = $env:CXX
            temp                = $Temp
        }
        tools           = [ordered]@{
            python           = [ordered]@{ path = $pythonExe; version = Get-ToolVersion $pythonExe }
            meson            = $mesonVersion
            ninja            = [ordered]@{ path = $ninjaExe; version = Get-ToolVersion $ninjaExe }
            cl               = if ($clangTools) { $null } else { [ordered]@{ path = (Get-Command cl.exe).Source; version = Get-ClVersion } }
            link             = if ($clangTools) { $null } else { [ordered]@{ path = (Get-Command link.exe).Source; version = Get-LinkVersion } }
            clang_cl         = if ($clangTools) { [ordered]@{ path = $clangTools['clang-cl']; version = Get-ToolVersion $clangTools['clang-cl'] } } else { $null }
            lld_link         = if ($clangTools) { [ordered]@{ path = $clangTools['lld-link']; version = Get-ToolVersion $clangTools['lld-link'] } } else { $null }
            llvm_lib         = if ($clangTools) { [ordered]@{ path = $clangTools['llvm-lib'] } } else { $null }
            glslangValidator = [ordered]@{ path = $glslangExe; version = Get-ToolVersion $glslangExe }
            widl             = [ordered]@{ path = $widlExe; version = Get-ToolVersion $widlExe @('-V'); probe = $widlProbe; idl_steps = $null }
        }
        gate            = $null
    }
    Write-Recipe $recipePath $recipe
    if ($clangTools) {
        Write-Host "compiler: clang-cl, $($recipe.tools.clang_cl.version) ($($clangTools['clang-cl']))"
        Write-Host "linker: $($recipe.tools.lld_link.version) ($($clangTools['lld-link'])), archiver $($clangTools['llvm-lib'])"
    } else {
        Write-Host "compiler: msvc, cl $($recipe.tools.cl.version) ($($recipe.tools.cl.path))"
        Write-Host "linker: link $($recipe.tools.link.version) ($($recipe.tools.link.path))"
    }

    Invoke-Checked $pythonExe $mesonArgs

    # The Build Options line ends with the native file (meson cmdline.py); it is not an option of the set.
    $logged = @(Get-MesonBuildOptions $Build | Where-Object { $_ -ne "--native-file=$nativeFile" })
    # Each IDL step of build.ninja must run the widl of the native file, which the probe checked. meson logs no path
    # for a program of a machine file, so the gate reads the commands that ninja runs.
    $idlTools = @{}
    $idlStep = $false
    foreach ($line in Get-Content -LiteralPath (Join-Path $Build 'build.ninja')) {
        if ($line -match '^build .*: CUSTOM_COMMAND \S+\.idl(\s|$)') { $idlStep = $true; continue }
        if ($idlStep -and $line -match '^\s+COMMAND = "?([^" ]+)') {
            $tool = [IO.Path]::GetFullPath($Matches[1])
            $idlTools[$tool] = 1 + $(if ($idlTools.ContainsKey($tool)) { $idlTools[$tool] } else { 0 })
            $idlStep = $false
        }
    }
    $recipe.tools.widl.idl_steps = $idlTools
    if ($idlTools.Count -ne 1 -or -not $idlTools.ContainsKey($widlExe)) {
        Write-Recipe $recipePath $recipe
        throw "the IDL steps of $Build\build.ninja run $(@($idlTools.Keys) -join ', '), not only $widlExe of $nativeFile"
    }
    Write-Host "widl: $widlExe ($($recipe.tools.widl.version)), explicit aggregate returns, in $($idlTools[$widlExe]) IDL step(s)"
    $missing = @($options | Where-Object { $logged -notcontains $_ })
    $extra = @($logged | Where-Object { $options -notcontains $_ })
    $recipe.gate = [ordered]@{
        logged_build_options = $logged
        same_set             = (-not $missing.Count -and -not $extra.Count)
        same_order           = (($logged -join "`n") -ceq ($options -join "`n"))
        missing              = $missing
        extra                = $extra
    }
    $recipe.status = if ($recipe.gate.same_set) { 'configured' } else { 'configure-gate-failed' }
    Write-Recipe $recipePath $recipe
    if (-not $recipe.gate.same_set) {
        throw "meson recorded different options. Missing: $($missing -join ' ') Extra: $($extra -join ' ')"
    }
    Write-Host "configure gate: the Build Options line matches the $Config option set ($($options.Count) options)"

    # Every compile command names the i686 target and the clang-cl x86 policy, and none removes the policy
    # (Test-ClangX86Policy, common.ps1).
    if ($clangTarget) {
        $recipe.gate['x86_policy'] = Test-ClangX86Policy $Build $clangTarget
        Write-Recipe $recipePath $recipe
        Assert-ClangX86Policy $recipe.gate['x86_policy'] $Build
    }

    if ($ConfigureOnly) { Write-Host "configured only: $Build (recipe: $recipePath)"; return }

    # -d keeprsp: the response file of each link step stays in the build directory, as the record of what the link
    # read. ninja deletes it after a link that succeeds; the outputs are the same with and without it.
    $ninjaArgs = @('-C', $Build, '-d', 'keeprsp')
    if ($Jobs -gt 0) { $ninjaArgs += "-j$Jobs" }
    Invoke-Checked $ninjaExe ($ninjaArgs + $targets)

    # The DLLs each target imports, the delay-loaded ones too. With the static C runtime, an import of a runtime DLL
    # stops the build before anything is stamped: the engine runs inside every D3D12 game, and a game that ships an
    # older msvcp140.dll beside its exe hands that copy to every DLL that imports the name (docs/build.md, radv-mt).
    $imports = [ordered]@{}
    foreach ($t in $targets) {
        $f = Join-Path $Build ($t -replace '/', '\')
        if (-not (Test-Path -LiteralPath $f)) { throw "ninja succeeded but $f is missing" }
        $deps = @(& dumpbin.exe /nologo /dependents $f)
        if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents $f failed ($LASTEXITCODE)" }
        $imports[$t] = @($deps | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
        $crt = @($imports[$t] | Where-Object { $_ -match '^(msvcp|vcruntime|concrt|ucrtbase|api-ms-win-crt-)' })
        if ($staticCrt -and $crt.Count) {
            throw "$t imports a C or C++ runtime DLL although -Config $Config links the static C runtime: $($crt -join ', ')"
        }
    }

    # BD-166: every artifact a release ships carries its version resource from this build, its last step before the
    # receipt; the release copies the file and rewrites none of it (tools\release\build-release.ps1). The option set
    # says which names a target ships as, because the resource names the file it belongs to.
    $stamped = [ordered]@{}
    foreach ($t in $targets) {
        $names = @()
        # A config without a ships map (per-app) stamps nothing. The key is looked up before it is read, as
        # release_options is: under Set-StrictMode -Version 3.0 reading an absent key is an error, not $null.
        if ($recipeConfig.PSObject.Properties['ships'] -and $recipeConfig.ships.PSObject.Properties.Name -contains $t) {
            $names = @($recipeConfig.ships.$t)
        }
        if ($names.Count -eq 0) { continue }
        $built = Join-Path $Build ($t -replace '/', '\')
        if (-not (Test-Path -LiteralPath $built)) { throw "ninja succeeded but $built is missing" }
        foreach ($name in $names) {
            $copy = Join-Path (Split-Path -Parent $built) $name
            if ($copy -ne $built) { Copy-Item -LiteralPath $built -Destination $copy -Force }
            Set-Bc250VersionStamp -Repo $repo -Files @($copy) -Names @($name) -Version $Version -Flavour $Flavour -Python $Python
            $stamped[((Split-Path -Parent $t) + '/' + $name)] = $copy
        }
    }

    $artifacts = [ordered]@{}
    foreach ($t in $targets) {
        $f = Join-Path $Build ($t -replace '/', '\')
        if (-not (Test-Path -LiteralPath $f)) { throw "ninja succeeded but $f is missing" }
        $artifacts[$t] = [ordered]@{ bytes = (Get-Item -LiteralPath $f).Length; sha256 = Get-FileSha256 $f; imports = $imports[$t] }
    }
    foreach ($rel in $stamped.Keys) {
        $f = $stamped[$rel]
        $artifacts[$rel] = [ordered]@{ bytes = (Get-Item -LiteralPath $f).Length; sha256 = Get-FileSha256 $f
                                       stamp = [ordered]@{ name = (Split-Path -Leaf $f); flavour = $Flavour } }
    }
    $recipe['artifacts'] = $artifacts
    if ($stamped.Count) { $recipe['stamp'] = [ordered]@{ version = $Version; flavour = $Flavour } }
    $recipe.status = 'built'
    Write-Recipe $recipePath $recipe
    foreach ($t in $artifacts.Keys) { '{0}  {1}' -f $artifacts[$t].sha256, $t }
} finally {
    Restore-ProcessEnvironment $savedEnv
}
