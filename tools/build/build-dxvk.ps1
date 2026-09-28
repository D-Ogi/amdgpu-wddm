#Requires -Version 7.0
<#
.SYNOPSIS
Configures and builds DXVK with a recorded meson option set (dxvk-configs.json).

.DESCRIPTION
-Config picks the option set from dxvk-configs.json next to this script (the only copy of those options):

  per-app     upstream DXVK as application-local DLLs, the comparison path for the proposed M14 system D3D11
              driver (ADR 0017, D004 5 % bound)
  ddi-engine  bc250dxvk.dll, the engine behind the M14 system D3D10/11 DDI UMD; needs a checkout of the
              amdgpu-wddm/ddi-engine DXVK branch (enable_ddi_engine does not exist upstream)

The build is a native MSVC build, as the M12 E33 per-application packages were: vcvars64.bat, meson from
PYTHONPATH, ninja and glslangValidator on PATH. DXVK needs no WDK headers. After configure the script compares
the new log's "Build Options:" line with the option set and stops on any difference.

The source tree must be a DXVK checkout with its submodules initialized (git submodule update --init
--recursive); this script patches nothing and fetches nothing.

.EXAMPLE
pwsh tools\build\build-dxvk.ps1 -Config per-app -Source P:\BC-250\scratch\m14\dxvk -Build P:\BC-250\scratch\m14\dxvk-per-app
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
    # ninja.exe; its directory goes in front of PATH. Default: whatever PATH gives.
    [string]$Ninja,
    [string]$VsInstall,
    # TEMP and TMP for the build (default <BC250_ROOT>\scratch\tmp).
    [string]$Temp
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')

$configsPath = Join-Path $PSScriptRoot 'dxvk-configs.json'
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

if (-not (Test-Path -LiteralPath (Join-Path $Source 'src\dxvk\dxvk_device.h'))) { throw "$Source is not a DXVK source tree" }
# An uninitialized submodule is an empty directory; meson would fail much later with a less useful message.
foreach ($sub in 'include\vulkan\include', 'include\spirv\include', 'subprojects\dxbc-spirv\meson.build', 'subprojects\libdisplay-info\meson.build') {
    if (-not (Test-Path -LiteralPath (Join-Path $Source $sub))) {
        throw "$Source\$sub is missing: run git -C $Source submodule update --init --recursive"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $Glslang 'glslangValidator.exe'))) { throw "glslangValidator.exe not found in ${Glslang}: pass -Glslang" }
$configured = Test-Path -LiteralPath (Join-Path $Build 'meson-private\coredata.dat')
if ($configured -and -not $Reconfigure) { throw "$Build is already configured: pass -Reconfigure, or use a new build directory" }

function Get-MesonBuildOptions([string]$BuildDir) {
    $log = Join-Path $BuildDir 'meson-logs\meson-log.txt'
    $line = Get-Content -LiteralPath $log -TotalCount 40 | Where-Object { $_.StartsWith('Build Options: ') } | Select-Object -First 1
    if (-not $line) { throw "no 'Build Options:' line in $log" }
    return @([regex]::Matches($line.Substring('Build Options: '.Length), "'[^']*'|\S+") | ForEach-Object { $_.Value.Trim("'") })
}

# Submodule commits are part of the source identity: DXVK's shader compiler lives in dxbc-spirv.
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
    $env:PYTHONPATH = $PythonPath

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
        recipe          = 'dxvk'
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
            pythonpath          = $PythonPath
            temp                = $Temp
        }
        tools           = [ordered]@{
            python           = [ordered]@{ path = $pythonExe; version = Get-ToolVersion $pythonExe }
            meson            = $mesonVersion
            ninja            = [ordered]@{ path = $ninjaExe; version = Get-ToolVersion $ninjaExe }
            cl               = [ordered]@{ path = (Get-Command cl.exe).Source; version = Get-ClVersion }
            glslangValidator = [ordered]@{ path = $glslangExe; version = Get-ToolVersion $glslangExe }
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
