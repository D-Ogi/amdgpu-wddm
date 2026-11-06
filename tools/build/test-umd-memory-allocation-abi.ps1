param([Parameter(Mandatory)][string]$MesaSource, [string]$OutputDir, [string]$VsInstall,
      [ValidateSet('x64', 'x86')][string]$Arch = 'x64',
      [ValidateSet('clang-cl', 'msvc')][string]$Compiler = 'clang-cl', [string]$MsvcReason = '')
# BD-309: the shell's copy of the hosted ICD's memory allocation query (driver/umd/dxvk/engine-memory-allocation.h)
# against the Mesa header it copies, src/amd/vulkan/radv_bc250_memory.h of -MesaSource (the mesa-amdgpu-wddm tree
# whose ICD the release ships). Both headers take the Vulkan headers of -MesaSource\include. -Compiler as in the
# other host tests (compiler policy); the script writes compiler.json into -OutputDir.
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
. "$PSScriptRoot\umd-compiler.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
$MesaSource=[IO.Path]::GetFullPath($MesaSource)
$mesaHeader=Join-Path $MesaSource 'src\amd\vulkan\radv_bc250_memory.h'
if (-not (Test-Path -LiteralPath $mesaHeader -PathType Leaf)) { throw "No radv_bc250_memory.h in $MesaSource" }
if (-not $OutputDir) { $OutputDir=Join-Path $root 'scratch\build\umd-memory-allocation-abi' }
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$saved=Save-ProcessEnvironment
try {
    $env:TEMP=$OutputDir; $env:TMP=$OutputDir
    $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
    $cc=Resolve-Bc250UmdCompiler $root $Compiler $MsvcReason $OutputDir $Arch
    $headers=@("$MesaSource\include", "$MesaSource\src\amd\vulkan")
    $flags=Get-Bc250HostTestFlags $Compiler @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/MD', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN') $headers $Arch @($repo)
    Push-Location $OutputDir
    try {
        Invoke-Bc250UmdCompiler $cc @flags /Fe:memory-allocation-abi-test.exe "$repo\driver\umd\dxvk\engine-memory-allocation-abi-test.cpp"
        if ($LASTEXITCODE -ne 0) { throw 'Memory-allocation ABI compilation failed' }
        & .\memory-allocation-abi-test.exe
        if ($LASTEXITCODE -ne 0) { throw 'Memory-allocation ABI test failed' }
    } finally { Pop-Location }
} finally { Restore-ProcessEnvironment $saved }
