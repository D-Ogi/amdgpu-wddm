#Requires -Version 7.0
<#
.SYNOPSIS
Configures and builds one of the lab's Mesa components with its recorded meson options (docs/build.md).

.DESCRIPTION
-Config picks the option set from mesa-configs.json next to this script (the only copy of those options):

  radv          RADV Vulkan ICD (vulkan_radeon.dll)
  radv-mt       the same with the static C runtime (-Db_vscrt=mt), for ICDs loaded into games
  llvmpipe-umd  desktop D3D10 UMD on llvmpipe (bc250d3d.dll), needs -Llvm
  zink-umd      native D3D10 UMD on Zink (bc250d3d_zink.dll)
  zink-gl       OpenGL on Zink (libgallium_wgl.dll, opengl32.dll)

The environment is the one the recorded cmd scripts set up: vcvars64.bat, the WDK NuGet um/shared headers in
front of INCLUDE, win_flex/win_bison and glslangValidator on PATH, meson and mako from PYTHONPATH, and for
llvmpipe the LLVM_CONFIG variable naming the llvm-config.exe of the LLVM build tree. After configure the
script compares the new log's "Build Options:" line with the option set and stops on any difference.

The source tree must already carry the component's patches (the companion Mesa repository branch, or an
upstream checkout with the experiment's patch applied); this script does not patch anything.

.EXAMPLE
pwsh tools\build\build-mesa.ps1 -Config radv -Source P:\BC-250\mesa-wddm -Build P:\BC-250\scratch\radv-build

.EXAMPLE
pwsh tools\build\build-mesa.ps1 -Config llvmpipe-umd -Source <tree> -Build <dir> -Llvm P:\BC-250\scratch\llvm2312-build
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('radv', 'radv-mt', 'llvmpipe-umd', 'zink-umd', 'zink-gl')][string]$Config,
    [Parameter(Mandatory)][string]$Source,
    [Parameter(Mandatory)][string]$Build,
    # Directory whose bin\llvm-config.exe describes the LLVM to link: the -Build directory of build-llvm.ps1.
    [string]$Llvm,
    [switch]$ConfigureOnly,
    # Re-run meson on an already configured build directory (meson setup --reconfigure).
    [switch]$Reconfigure,
    # Parallel jobs for ninja; 0 leaves the choice to ninja.
    [int]$Jobs = 0,
    # Unpacked WDK/SDK NuGet packages (default <BC250_ROOT>\toolchain\nuget) and the kit version inside them.
    [string]$Kits,
    [string]$KitVersion = '10.0.26100.0',
    # Directory holding meson, mako, markupsafe, PyYAML (and packaging) for PYTHONPATH (default <BC250_ROOT>\scratch\py).
    [string]$PythonPath,
    # python.exe that runs meson; default: python on PATH.
    [string]$Python,
    # win_flex/win_bison directory (default <BC250_ROOT>\toolchain\winflexbison-2.5.25).
    [string]$FlexBison,
    # Directory holding glslangValidator.exe (default <BC250_ROOT>\scratch\glslang\bin); required for radv.
    [string]$Glslang,
    # ninja.exe; its directory goes in front of PATH. Default: whatever PATH gives.
    [string]$Ninja,
    [string]$VsInstall,
    # TEMP and TMP for the build (default <BC250_ROOT>\scratch\tmp).
    [string]$Temp,
    # Target architecture: x86 builds the 32-bit (WoW64) DLL with vcvarsamd64_x86.bat (common.ps1).
    [ValidateSet('x64', 'x86')][string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$configsPath = Join-Path $PSScriptRoot 'mesa-configs.json'
$recipeConfig = (Get-Content -LiteralPath $configsPath -Raw | ConvertFrom-Json).configs.$Config
$options = @($recipeConfig.options)
$targets = @($recipeConfig.targets)

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Get-Bc250Root $repo
if (-not $Kits) { $Kits = Join-Path $root 'toolchain\nuget' }
if (-not $PythonPath) { $PythonPath = Join-Path $root 'scratch\py' }
if (-not $FlexBison) { $FlexBison = Join-Path $root 'toolchain\winflexbison-2.5.25' }
if (-not $Glslang) { $Glslang = Join-Path $root 'scratch\glslang\bin' }
if (-not $Temp) { $Temp = Join-Path $root 'scratch\tmp' }
$Source = [IO.Path]::GetFullPath($Source)
$Build = [IO.Path]::GetFullPath($Build)

if (-not (Test-Path -LiteralPath (Join-Path $Source 'meson.build'))) { throw "$Source is not a Mesa source tree (no meson.build)" }
$llvmConfig = $null
if ($Llvm) {
    $llvmConfig = Join-Path ([IO.Path]::GetFullPath($Llvm)) 'bin\llvm-config.exe'
    if (-not (Test-Path -LiteralPath $llvmConfig)) {
        throw "$llvmConfig does not exist. -Llvm takes the LLVM BUILD directory: the install prefix has no llvm-config.exe"
    }
} elseif ($recipeConfig.needs_llvm) {
    throw "-Config $Config links LLVM: pass -Llvm <LLVM build directory> (build-llvm.ps1 -Build)"
}
$wdkInclude = Join-Path $Kits "microsoft.windows.wdk.x64\c\Include\$KitVersion"
foreach ($d in (Join-Path $wdkInclude 'um'), (Join-Path $wdkInclude 'shared'), $FlexBison) {
    if (-not (Test-Path -LiteralPath $d)) { throw "$d does not exist (see docs/build.md, Prerequisites)" }
}
$configured = Test-Path -LiteralPath (Join-Path $Build 'meson-private\coredata.dat')
if ($configured -and -not $Reconfigure) { throw "$Build is already configured: pass -Reconfigure, or use a new build directory" }

function Get-MesonBuildOptions([string]$BuildDir) {
    $log = Join-Path $BuildDir 'meson-logs\meson-log.txt'
    $line = Get-Content -LiteralPath $log -TotalCount 40 | Where-Object { $_.StartsWith('Build Options: ') } | Select-Object -First 1
    if (-not $line) { throw "no 'Build Options:' line in $log" }
    # meson quotes an argument with shell metacharacters in single quotes ('-Dgallium-drivers=[]').
    return @([regex]::Matches($line.Substring('Build Options: '.Length), "'[^']*'|\S+") | ForEach-Object { $_.Value.Trim("'") })
}

# What meson probed and found. Kept in the recipe so that two builds' environments can be compared.
function Get-MesonProbes([string]$BuildDir) {
    $log = Join-Path $BuildDir 'meson-logs\meson-log.txt'
    return @(Get-Content -LiteralPath $log | Where-Object {
            $_ -match '^(Program|Dependency|Run-time dependency|Found (ninja|pkg-config|CMake)|llvm-config found)\b' -and
            $_ -match '\b(found|skipped|Found)\b' -and $_ -notmatch '^Dependency lookup'
        } | Sort-Object -Unique)
}

$recipePath = Join-Path $Build 'recipe.json'
$savedEnv = Save-ProcessEnvironment
try {
    $vs = Import-VsDevEnvironment $VsInstall $Temp $Arch
    New-Item -ItemType Directory -Force $Temp, $Build | Out-Null
    $env:TEMP = $Temp
    $env:TMP = $Temp
    # glslangValidator goes on PATH as it did in the recorded build of this configuration (mesa-configs.json).
    $pathFront = @()
    $haveGlslang = Test-Path -LiteralPath (Join-Path $Glslang 'glslangValidator.exe')
    if ($recipeConfig.glslang -eq 'required' -and -not $haveGlslang) {
        throw "glslangValidator.exe not found in $Glslang (required by -Config $Config): pass -Glslang"
    }
    if ($haveGlslang -and $recipeConfig.glslang -ne 'not-on-path') { $pathFront += $Glslang }
    if ($llvmConfig) { $pathFront += (Split-Path -Parent $llvmConfig) }
    if ($Ninja) { $pathFront += (Split-Path -Parent (Resolve-Tool $Ninja 'ninja')) }
    $pathFront += $FlexBison
    Add-PathFront $pathFront
    $includeFront = @((Join-Path $wdkInclude 'um'), (Join-Path $wdkInclude 'shared'))
    $env:INCLUDE = ($includeFront + @($env:INCLUDE)) -join ';'
    $env:PYTHONPATH = $PythonPath
    if ($llvmConfig) { $env:LLVM_CONFIG = $llvmConfig }

    $pythonExe = Resolve-Tool $Python 'python'
    if (-not $pythonExe) { throw 'python not found: put it on PATH or pass -Python' }
    $probe = Invoke-Capture $pythonExe @('-c', 'import mesonbuild.coredata as c, mako, yaml, packaging; print(c.version); print(mako.__version__)')
    if ($probe.Code -ne 0) {
        throw ("$pythonExe cannot import meson, mako, PyYAML and packaging from PYTHONPATH=$PythonPath. " +
            "Install them there: python -m pip install --target $PythonPath meson==1.12.0 mako==1.4.1 pyyaml==6.0.3 packaging==25.0`n$($probe.Err)")
    }
    $mesonVersion, $makoVersion = @($probe.Out -split "`r?`n" | Where-Object { $_ })
    $ninjaExe = Resolve-Tool '' 'ninja'
    if (-not $ninjaExe) { throw 'ninja not found: put it on PATH or pass -Ninja' }

    $mesonArgs = @('-m', 'mesonbuild.mesonmain', 'setup')
    if ($configured) { $mesonArgs += '--reconfigure' }
    $mesonArgs += @($Build, $Source) + $options

    $flex = Join-Path $FlexBison 'win_flex.exe'
    $bison = Join-Path $FlexBison 'win_bison.exe'
    $glslangExe = Resolve-Tool '' 'glslangValidator'
    $recipe = [ordered]@{
        recipe          = 'mesa'
        schema          = 1
        config          = $Config
        description     = $recipeConfig.description
        utc             = [DateTime]::UtcNow.ToString('o')
        status          = 'configuring'
        recipe_identity = Get-RecipeIdentity $repo @($PSCommandPath, $configsPath, (Join-Path $PSScriptRoot 'common.ps1'))
        bc250_root      = $root
        source          = Get-SourceIdentity $Source
        build_dir       = $Build
        meson_options   = $options
        ninja_targets   = $targets
        environment     = [ordered]@{
            vs_install          = $vs
            arch                = $Arch
            vc_tools_version    = $env:VCToolsVersion
            windows_sdk_version = if ($env:WindowsSDKVersion) { $env:WindowsSDKVersion.TrimEnd('\') } else { $null }
            path_prepended      = $pathFront
            include_prepended   = $includeFront
            pythonpath          = $PythonPath
            llvm_config         = $llvmConfig
            temp                = $Temp
        }
        tools           = [ordered]@{
            python          = [ordered]@{ path = $pythonExe; version = Get-ToolVersion $pythonExe }
            meson           = $mesonVersion
            mako            = $makoVersion
            ninja           = [ordered]@{ path = $ninjaExe; version = Get-ToolVersion $ninjaExe }
            cl              = [ordered]@{ path = (Get-Command cl.exe).Source; version = Get-ClVersion }
            win_flex        = Get-ToolVersion $flex
            win_bison       = Get-ToolVersion $bison
            glslangValidator = if ($glslangExe) { [ordered]@{ path = $glslangExe; version = Get-ToolVersion $glslangExe } } else { $null }
            llvm_config     = if ($llvmConfig) { (Invoke-Capture $llvmConfig @('--version')).Out.Trim() } else { $null }
        }
        gate            = $null
        probes          = $null
    }
    Write-Recipe $recipePath $recipe

    Invoke-Checked $pythonExe $mesonArgs

    # The gate: meson must have recorded exactly the option set, nothing more, nothing less.
    $logged = Get-MesonBuildOptions $Build
    $missing = @($options | Where-Object { $logged -notcontains $_ })
    $extra = @($logged | Where-Object { $options -notcontains $_ })
    $recipe.gate = [ordered]@{
        logged_build_options = $logged
        same_set             = (-not $missing.Count -and -not $extra.Count)
        same_order           = (($logged -join "`n") -ceq ($options -join "`n"))
        missing              = $missing
        extra                = $extra
    }
    $recipe.probes = Get-MesonProbes $Build
    $recipe.status = if ($recipe.gate.same_set) { 'configured' } else { 'configure-gate-failed' }
    Write-Recipe $recipePath $recipe
    if (-not $recipe.gate.same_set) {
        throw "meson recorded different options. Missing: $($missing -join ' ') Extra: $($extra -join ' ')"
    }
    Write-Host "configure gate: the Build Options line matches the $Config option set ($($options.Count) options)"

    if ($ConfigureOnly) { Write-Host "configured only: $Build (recipe: $recipePath)"; return }

    $ninjaArgs = @('-C', $Build)
    if ($Jobs -gt 0) { $ninjaArgs += "-j$Jobs" }
    Invoke-Checked $ninjaExe ($ninjaArgs + $targets)

    $artifacts = [ordered]@{}
    foreach ($t in $targets) {
        $f = Join-Path $Build ($t -replace '/', '\')
        if (-not (Test-Path -LiteralPath $f)) { throw "ninja succeeded but $f is missing" }
        $artifacts[$t] = [ordered]@{ bytes = (Get-Item -LiteralPath $f).Length; sha256 = Get-FileSha256 $f }
    }
    $recipe['artifacts'] = $artifacts
    $recipe.status = 'built'
    Write-Recipe $recipePath $recipe
    foreach ($t in $artifacts.Keys) { '{0}  {1}' -f $artifacts[$t].sha256, $t }
} finally {
    Restore-ProcessEnvironment $savedEnv
}
