#Requires -Version 7.0
<#
.SYNOPSIS
Configures and builds vkd3d-proton with a recorded meson option set (vkd3d-configs.json).

.DESCRIPTION
-Config picks the option set from vkd3d-configs.json next to this script (the only copy of those options):

  per-app     upstream vkd3d-proton as application-local d3d12.dll and d3d12core.dll (M12)
  ddi-engine  bc250vkd3d.dll, the engine behind the proposed system D3D12 DDI UMD (G4), and its offline test;
              needs a checkout of the amdgpu-wddm/ddi-engine vkd3d-proton branch (enable_ddi_engine does not
              exist upstream)

The build is a native MSVC build, like the M12 per-application packages: vcvars64.bat, meson from PYTHONPATH,
ninja and glslang on PATH, CC and CXX set to cl. vkd3d-proton also needs the IDL compiler widl (MSYS2
mingw64 ships one); its directory goes at the end of PATH, so that nothing else in it shadows the MSVC tools. After configure the script compares the new log's "Build Options:" line with the option set and
stops on any difference.

The source tree must be a vkd3d-proton checkout with its submodules initialized (git submodule update --init
--recursive); this script patches nothing and fetches nothing.

.EXAMPLE
pwsh tools\build\build-vkd3d.ps1 -Config ddi-engine -Source P:\BC-250\scratch\m15\vkd3d -Build P:\BC-250\scratch\m15\vkd3d-ddi-engine -Widl C:\msys64\mingw64\bin\widl.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('per-app', 'ddi-engine')][string]$Config,
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
    # widl.exe; default: whatever PATH gives.
    [string]$Widl,
    # ninja.exe; its directory goes in front of PATH. Default: whatever PATH gives.
    [string]$Ninja,
    [string]$VsInstall,
    # TEMP and TMP for the build (default <BC250_ROOT>\scratch\tmp).
    [string]$Temp
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$configsPath = Join-Path $PSScriptRoot 'vkd3d-configs.json'
$recipeConfig = (Get-Content -LiteralPath $configsPath -Raw | ConvertFrom-Json).configs.$Config
$options = @($recipeConfig.options)
$targets = @($recipeConfig.targets)

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$root = Get-Bc250Root $repo
if (-not $PythonPath) { $PythonPath = Join-Path $root 'scratch\py' }
if (-not $Glslang) { $Glslang = Join-Path $root 'scratch\glslang\bin' }
if (-not $Temp) { $Temp = Join-Path $root 'scratch\tmp' }
$Source = [IO.Path]::GetFullPath($Source)
$Build = [IO.Path]::GetFullPath($Build)

if (-not (Test-Path -LiteralPath (Join-Path $Source 'libs\vkd3d\vkd3d_private.h'))) { throw "$Source is not a vkd3d-proton source tree" }
# An uninitialized submodule is an empty directory; meson would fail much later with a less useful message.
foreach ($sub in 'khronos\Vulkan-Headers\include', 'khronos\SPIRV-Headers\include', 'subprojects\dxil-spirv\meson.build',
        'subprojects\dxil-spirv\third_party\SPIRV-Cross\CMakeLists.txt') {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $sub))) {
        throw "$Source\$sub is missing: run git -C $Source submodule update --init --recursive"
    }
}
if ($Config -eq 'ddi-engine' -and -not (Test-Path -LiteralPath (Join-Path $Source 'libs\ddi\bc250_vkd3d_engine.h'))) {
    throw "$Source has no libs\ddi: -Config ddi-engine needs the amdgpu-wddm/ddi-engine branch"
}
if (-not (Test-Path -LiteralPath (Join-Path $Glslang 'glslangValidator.exe'))) { throw "glslangValidator.exe not found in ${Glslang}: pass -Glslang" }
$widlExe = Resolve-Tool $Widl 'widl'
if (-not $widlExe) { throw 'widl not found: put it on PATH or pass -Widl (MSYS2 mingw64 ships it)' }
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
    $vs = Import-VsDevEnvironment $VsInstall $Temp
    New-Item -ItemType Directory -Force $Temp, $Build | Out-Null
    $env:TEMP = $Temp
    $env:TMP = $Temp
    $pathFront = @($Glslang)
    if ($Ninja) { $pathFront += (Split-Path -Parent (Resolve-Tool $Ninja 'ninja')) }
    Add-PathFront $pathFront
    $widlDir = Split-Path -Parent $widlExe
    $env:PATH = "$env:PATH;$widlDir"
    $env:PYTHONPATH = $PythonPath
    # Without these, meson may take gcc from a MinGW directory on PATH.
    $env:CC = 'cl'
    $env:CXX = 'cl'

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
    $mesonArgs += @($Build, $Source) + $options

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
        ninja_targets   = $targets
        environment     = [ordered]@{
            vs_install          = $vs
            vc_tools_version    = $env:VCToolsVersion
            windows_sdk_version = if ($env:WindowsSDKVersion) { $env:WindowsSDKVersion.TrimEnd('\') } else { $null }
            path_prepended      = $pathFront
            path_appended       = @($widlDir)
            pythonpath          = $PythonPath
            cc                  = $env:CC
            cxx                 = $env:CXX
            temp                = $Temp
        }
        tools           = [ordered]@{
            python           = [ordered]@{ path = $pythonExe; version = Get-ToolVersion $pythonExe }
            meson            = $mesonVersion
            ninja            = [ordered]@{ path = $ninjaExe; version = Get-ToolVersion $ninjaExe }
            cl               = [ordered]@{ path = (Get-Command cl.exe).Source; version = Get-ClVersion }
            glslangValidator = [ordered]@{ path = $glslangExe; version = Get-ToolVersion $glslangExe }
            widl             = [ordered]@{ path = $widlExe; version = Get-ToolVersion $widlExe @('-V') }
        }
        gate            = $null
    }
    Write-Recipe $recipePath $recipe

    Invoke-Checked $pythonExe $mesonArgs

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
