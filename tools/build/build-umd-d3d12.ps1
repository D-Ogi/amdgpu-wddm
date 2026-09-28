param([string]$OutputDir,[string]$VsInstall,[string]$MesaSource,[string]$VulkanInclude,[string]$EngineInclude)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if(!$EngineInclude){$EngineInclude=Join-Path $root 'scratch\m15\vkd3d-1.2-src\libs\ddi'}
if(!$MesaSource){$MesaSource=Join-Path $root 'scratch\m12\mesa-current-src'}
if(!$VulkanInclude){$VulkanInclude=Join-Path $root 'scratch\m15\vkd3d\khronos\Vulkan-Headers\include'}
if(!$OutputDir){$OutputDir=Join-Path $root 'scratch\build\d3d12-adapter'}
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir|Out-Null
$engineSource=Split-Path -Parent (Split-Path -Parent $EngineInclude)
$engineBuild=Join-Path $OutputDir 'engine-ddi'
$engineArgs=@('-NoProfile','-File',"$PSScriptRoot\build-engine-ddi.ps1",'-OutputDir',$engineBuild,'-EngineSource',$engineSource,'-VulkanInclude',$VulkanInclude,'-NativeOnly')
if($VsInstall){$engineArgs+=@('-VsInstall',$VsInstall)}
& pwsh @engineArgs
if($LASTEXITCODE){throw 'Native engine-ddi build or host gates failed'}
$engineLib=Join-Path $engineBuild 'engine-ddi.lib'
if(!(Test-Path -LiteralPath $engineLib)){throw 'Native engine-ddi library missing'}
$saved=Save-ProcessEnvironment
try {
 $env:TEMP=$OutputDir;$env:TMP=$OutputDir
 $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir
 $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
 $flags=@('/nologo','/std:c++20','/EHsc','/W4','/WX','/external:W0','/MT','/DNOMINMAX',"/external:I$VulkanInclude","/external:I$EngineInclude","/external:I$MesaSource\src\util","/external:I$wdk","/external:I$wdk\..\shared", "/I$repo\driver\contract\third_party", "/I$repo\driver\contract\uapi-shim")
 Push-Location $OutputDir
 try {
  if(Test-Path amdgpu_wddm_d3d12.dll){
   $hash=(Get-FileHash amdgpu_wddm_d3d12.dll).Hash
   Copy-Item amdgpu_wddm_d3d12.dll "retained-$hash.dll"
  }
  & cl.exe @flags /LD /Fe:amdgpu_wddm_d3d12.dll "$repo\driver\umd\d3d12\adapter.cpp" "$repo\driver\umd\d3d12\adapter-caps.cpp" $engineLib
  if($LASTEXITCODE){throw 'Adapter build failed'}
  & cl.exe @flags /Fe:adapter-test.exe "$repo\driver\umd\d3d12\adapter-test.cpp"
  if($LASTEXITCODE){throw 'Test build failed'}
  & .\adapter-test.exe (Join-Path $OutputDir 'amdgpu_wddm_d3d12.dll')
  if($LASTEXITCODE){throw 'Adapter tests failed'}
  & cl.exe @flags /Fe:queue-context-test.exe "$repo\driver\umd\d3d12\queue-context-test.cpp"
  if($LASTEXITCODE){throw 'Queue context test build failed'}
  & .\queue-context-test.exe
  if($LASTEXITCODE){throw 'Queue context tests failed'}
  & cl.exe @flags "/I$repo\driver\contract\third_party" "/I$repo\driver\contract\uapi-shim" /Fe:queue-request-test.exe "$repo\driver\umd\d3d12\queue-request-test.cpp"
  if($LASTEXITCODE){throw 'Queue request build failed'}
  & .\queue-request-test.exe
  if($LASTEXITCODE){throw 'Queue request tests failed'}
  & cl.exe @flags "/I$repo\driver\contract\third_party" "/I$repo\driver\contract\uapi-shim" /Fe:queue-registry-test.exe "$repo\driver\umd\d3d12\queue-registry-test.cpp"
  if($LASTEXITCODE){throw 'Queue registry build failed'}
  & .\queue-registry-test.exe
  if($LASTEXITCODE){throw 'Queue registry tests failed'}
  & cl.exe @flags "/I$repo\driver\contract\third_party" "/I$repo\driver\contract\uapi-shim" /Fe:queue-ddi-test.exe "$repo\driver\umd\d3d12\queue-ddi-test.cpp"
  if($LASTEXITCODE){throw 'Queue DDI build failed'}
  & .\queue-ddi-test.exe
  if($LASTEXITCODE){throw 'Queue DDI tests failed'}
  & cl.exe @flags /Fe:fence-ddi-test.exe "$repo\driver\umd\d3d12\fence-ddi-test.cpp"
  if($LASTEXITCODE){throw 'Fence DDI test build failed'}
  & .\fence-ddi-test.exe
  if($LASTEXITCODE){throw 'Fence DDI tests failed'}
  & cl.exe @flags /Fe:allocation-test.exe "$repo\driver\umd\d3d12\allocation-test.cpp"
  if($LASTEXITCODE){throw 'Allocation ownership test build failed'}
  & .\allocation-test.exe
  if($LASTEXITCODE){throw 'Allocation ownership tests failed'}
  & cl.exe @flags /Fe:allocation-request-test.exe "$repo\driver\umd\d3d12\allocation-request-test.cpp" "$repo\driver\kmd\umd_blob.c"
  if($LASTEXITCODE){throw 'Allocation request test build failed'}
  & .\allocation-request-test.exe
  if($LASTEXITCODE){throw 'Allocation request tests failed'}
  & cl.exe @flags /Fe:paging-test.exe "$repo\driver\umd\d3d12\paging-test.cpp"
  if($LASTEXITCODE){throw 'Paging test build failed'}
  & .\paging-test.exe
  if($LASTEXITCODE){throw 'Paging tests failed'}
  & cl.exe @flags /Fe:residency-test.exe "$repo\driver\umd\d3d12\residency-test.cpp"
  if($LASTEXITCODE){throw 'Residency test build failed'}
  & .\residency-test.exe
  if($LASTEXITCODE){throw 'Residency tests failed'}
  & cl.exe @flags /Fe:memory-registry-test.exe "$repo\driver\umd\d3d12\memory-registry-test.cpp"
  if($LASTEXITCODE){throw 'Memory registry test build failed'}
  & .\memory-registry-test.exe
  if($LASTEXITCODE){throw 'Memory registry tests failed'}
  & cl.exe @flags "/external:I$MesaSource\src\util" "/external:I$VulkanInclude" /Fe:adapter-query-scope-test.exe "$repo\driver\umd\d3d12\adapter-query-scope-test.cpp"
  if($LASTEXITCODE){throw 'Adapter query scope test build failed'}
  & .\adapter-query-scope-test.exe
  if($LASTEXITCODE){throw 'Adapter query scope tests failed'}
  & cl.exe @flags "/external:I$MesaSource\src\util" "/external:I$VulkanInclude" "/external:I$EngineInclude" /Fe:adapter-caps-probe.exe "$repo\driver\umd\d3d12\adapter-caps-probe.cpp" /link dxgi.lib
  if($LASTEXITCODE){throw 'Adapter caps probe build failed'}
  & .\adapter-caps-probe.exe --help
  if($LASTEXITCODE){throw 'Adapter caps probe help failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:device-table-test.exe "$repo\driver\umd\d3d12\device-table-test.cpp" $engineLib
  if($LASTEXITCODE){throw 'Device table composition test build failed'}
  & .\device-table-test.exe
  if($LASTEXITCODE){throw 'Device table composition tests failed'}
  & cl.exe @flags /Fe:adapter-kmt-probe.exe "$repo\driver\umd\d3d12\adapter-kmt-probe.cpp" /link dxgi.lib gdi32.lib
  if($LASTEXITCODE){throw 'Adapter KMT probe build failed'}
  & .\adapter-kmt-probe.exe --help
  if($LASTEXITCODE){throw 'Adapter KMT probe help failed'}
 } finally {Pop-Location}
} finally {Restore-ProcessEnvironment $saved}
Write-Host 'Adapter caps linked; functional device tables and deployment remain separate gates.'
