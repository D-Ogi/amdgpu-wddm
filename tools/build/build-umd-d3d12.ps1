param([string]$OutputDir,[string]$VsInstall)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if(!$OutputDir){$OutputDir=Join-Path $root 'scratch\build\d3d12-adapter'}
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir|Out-Null
$saved=Save-ProcessEnvironment
try {
 $env:TEMP=$OutputDir;$env:TMP=$OutputDir
 $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
 $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
 $flags=@('/nologo','/std:c++20','/EHsc','/W4','/WX','/external:W0','/MT','/DNOMINMAX',"/external:I$wdk","/external:I$wdk\..\shared")
 Push-Location $OutputDir
 try {
  if(Test-Path amdgpu_wddm_d3d12.dll){
   $hash=(Get-FileHash amdgpu_wddm_d3d12.dll).Hash
   Copy-Item amdgpu_wddm_d3d12.dll "retained-$hash.dll"
  }
  & cl.exe @flags /LD /Fe:amdgpu_wddm_d3d12.dll "$repo\driver\umd\d3d12\adapter.cpp"
  if($LASTEXITCODE){throw 'Adapter build failed'}
  & cl.exe @flags /Fe:adapter-test.exe "$repo\driver\umd\d3d12\adapter-test.cpp"
  if($LASTEXITCODE){throw 'Test build failed'}
  & .\adapter-test.exe (Join-Path $OutputDir 'amdgpu_wddm_d3d12.dll')
  if($LASTEXITCODE){throw 'Adapter tests failed'}
 } finally {Pop-Location}
} finally {Restore-ProcessEnvironment $saved}
Write-Host 'Diagnostic adapter only; no functional device or deployment.'
