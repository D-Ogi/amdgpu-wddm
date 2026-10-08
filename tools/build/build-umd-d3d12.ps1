# -Arch x86 builds the 32-bit shell for WoW64 processes (UserModeDriverNameWow) with vcvarsamd64_x86.bat (common.ps1),
# and runs every host gate below as an x86 program.
param([string]$OutputDir,[string]$VsInstall,[string]$MesaSource,[string]$VulkanInclude,[string]$EngineInclude,[ValidateSet('x64','x86')][string]$Arch='x64')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\common.ps1"
# Windows installer detection asks for elevation (a UAC prompt on the desktop of the build PC) before it starts a
# 32-bit exe without a manifest whose name holds "patch", "setup", "install" or "update". hosted-dispatch-test.exe
# below is one ("dis-patch"): the -Arch x86 build waited on two prompts on 2026-10-08. No host gate here needs
# elevation, so every child of this recipe starts as the invoker.
$env:__COMPAT_LAYER='RunAsInvoker'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$root=Get-Bc250Root $repo
if(!$EngineInclude){
 $pin=Get-Content (Join-Path $repo 'driver\umd\d3d12\engine-ddi\engine-abi.json') -Raw|ConvertFrom-Json
 $checkout=if($pin.source_checkout){$pin.source_checkout}else{'scratch/m15/vkd3d-1.2-src'}
 $EngineInclude=Join-Path (Join-Path $root $checkout) 'libs\ddi'
}
if(!$MesaSource){$MesaSource=Join-Path $root 'scratch\m12\mesa-current-src'}
if(!$VulkanInclude){$VulkanInclude=Join-Path $root 'scratch\m15\vkd3d\khronos\Vulkan-Headers\include'}
if(!$OutputDir){$OutputDir=Join-Path $root 'scratch\build\d3d12-adapter'}
# Full paths: /d1trimfile below cuts each of these prefixes from the source paths the compiler sees.
$MesaSource=[IO.Path]::GetFullPath($MesaSource);$VulkanInclude=[IO.Path]::GetFullPath($VulkanInclude);$EngineInclude=[IO.Path]::GetFullPath($EngineInclude)
$OutputDir=[IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force $OutputDir|Out-Null
$engineSource=Split-Path -Parent (Split-Path -Parent $EngineInclude)
$engineBuild=Join-Path $OutputDir 'engine-ddi'
$engineArgs=@('-NoProfile','-File',"$PSScriptRoot\build-engine-ddi.ps1",'-OutputDir',$engineBuild,'-EngineSource',$engineSource,'-VulkanInclude',$VulkanInclude,'-NativeOnly','-Arch',$Arch)
if($VsInstall){$engineArgs+=@('-VsInstall',$VsInstall)}
& pwsh @engineArgs
if($LASTEXITCODE){throw 'Native engine-ddi build or host gates failed'}
$engineLib=Join-Path $engineBuild 'engine-ddi.lib'
if(!(Test-Path -LiteralPath $engineLib)){throw 'Native engine-ddi library missing'}
# The recent-launch record that the adapter's CreateDevice notes (gate G-RG).
& "$PSScriptRoot\test-umd-recent-launch.ps1" -OutputDir (Join-Path $OutputDir 'quality\recent-launch') -VsInstall $VsInstall -Arch $Arch
# The per-application graphics settings that CreateDevice and Present read.
& "$PSScriptRoot\test-umd-app-settings.ps1" -OutputDir (Join-Path $OutputDir 'quality\app-settings') -VsInstall $VsInstall -Arch $Arch
$saved=Save-ProcessEnvironment
try {
 $env:TEMP=$OutputDir;$env:TMP=$OutputDir
 $null=Import-VsDevEnvironment -VsInstall $VsInstall -TempDir $OutputDir -Arch $Arch
 $wdk=Join-Path $root 'toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um'
 # /O2: the DLL and its host gates are optimised builds (until 2026-09-30 the line had no /O flag, so cl.exe
 # compiled at /Od; trial 164 attributed 4.3 ms/frame to scope bookkeeping alone).
 # /GL (whole-program optimisation, the linker runs LTCG on /GL objects) and /arch:AVX2 (the BC-250's Zen 2
 # and the development PC both have it) are the strongest level MSVC offers; there is no /O3 in cl.exe. Both
 # are withdrawn since 2026-09-30 until the game route renders clean twice on the /O2-only build: the only
 # /O2 /GL /arch:AVX2 game run (trial 171, adapter100) corrupted the player character with no error, and
 # the review (scratch/m15/entry-lock/CORRUPTION-ANALYSIS.md, local) found no cause; /O2 carries the measured
 # gain, the other two have none measured in a forwarding layer. They return one at a time with a measurement.
 # /FC with /d1trimfile: __FILE__ and the anonymous-namespace names see the path below each tree only.
 $flags=@('/nologo','/std:c++20','/EHsc','/W4','/WX','/O2','/external:W0','/MT','/DNOMINMAX','/Brepro','/FC',"/d1trimfile:$repo","/d1trimfile:$MesaSource","/d1trimfile:$VulkanInclude","/d1trimfile:$EngineInclude","/external:I$VulkanInclude","/external:I$EngineInclude","/external:I$MesaSource\src\util","/external:I$wdk","/external:I$wdk\..\shared", "/I$repo\driver\contract\third_party", "/I$repo\driver\contract\uapi-shim")
 Push-Location $OutputDir
 try {
  if(Test-Path amdgpu_wddm_d3d12.dll){
   $hash=(Get-FileHash amdgpu_wddm_d3d12.dll).Hash
   Copy-Item amdgpu_wddm_d3d12.dll "retained-$hash.dll"
  }
  & cl.exe @flags /LD /Fe:amdgpu_wddm_d3d12.dll "$repo\driver\umd\d3d12\adapter.cpp" "$repo\driver\umd\d3d12\adapter-caps.cpp" "$repo\driver\umd\d3d12\device-engine.cpp" "$repo\driver\umd\d3d12\hosted-dispatch.cpp" "$repo\driver\umd\d3d12\queue-engine.cpp" "$repo\driver\umd\d3d12\hosted-queue.cpp" "$repo\driver\umd\d3d12\heap-import.cpp" "$repo\driver\umd\d3d12\native-queue-ddi.cpp" "$repo\driver\umd\d3d12\native-residency-ddi.cpp" "$repo\driver\umd\d3d12\native-tables.cpp" $engineLib /link /Brepro /MAP:amdgpu_wddm_d3d12.map "/DEF:$repo\driver\umd\d3d12\amdgpu_wddm_d3d12.def"
  if($LASTEXITCODE){throw 'Adapter build failed'}
  # The runtime finds the shell by one export name, OpenAdapter12, on x64 and on x86 alike (the .def file keeps the
  # x86 stdcall decoration out of the name). The image's machine must be the one asked for.
  $exports=@(& dumpbin.exe /nologo /exports .\amdgpu_wddm_d3d12.dll|Where-Object{$_ -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)'}|ForEach-Object{$Matches[1]})
  if(($exports -join ',') -ne 'OpenAdapter12'){throw "Shell exports '$($exports -join ',')', wanted exactly OpenAdapter12"}
  $machine=(& dumpbin.exe /nologo /headers .\amdgpu_wddm_d3d12.dll|Select-String 'machine \(') -join ' '
  $wantMachine=@{x64='8664 machine (x64)';x86='14C machine (x86)'}[$Arch]
  if(!$machine.Contains($wantMachine)){throw "Shell image is '$machine', wanted $wantMachine"}
  # Static C runtime, as the D3D11 shell's recipe checks it: a game's own older msvcp140.dll must not reach the shell.
  $crt=@(& dumpbin.exe /nologo /dependents .\amdgpu_wddm_d3d12.dll|Where-Object{$_ -match '^\s+(msvcp|vcruntime|concrt|ucrtbase|api-ms-win-crt-)\S*\.dll\s*$'})
  if($crt.Count){throw ('Shell imports a dynamic C/C++ runtime: '+(($crt|ForEach-Object{$_.Trim()}) -join ', '))}
  & cl.exe @flags /Fe:adapter-test.exe "$repo\driver\umd\d3d12\adapter-test.cpp"
  if($LASTEXITCODE){throw 'Test build failed'}
  & .\adapter-test.exe (Join-Path $OutputDir 'amdgpu_wddm_d3d12.dll')
  if($LASTEXITCODE){throw 'Adapter tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:memory-policy-test.exe "$repo\driver\umd\d3d12\memory-policy-test.cpp"
  if($LASTEXITCODE){throw 'Memory policy query test build failed'}
  & .\memory-policy-test.exe
  if($LASTEXITCODE){throw 'Memory policy query tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:instance-policy-test.exe "$repo\driver\umd\d3d12\instance-policy-test.cpp"
  if($LASTEXITCODE){throw 'Instance policy query test build failed'}
  & .\instance-policy-test.exe
  if($LASTEXITCODE){throw 'Instance policy query tests failed'}
  # Built only: the probe asks the BC-250's adapter key and belongs on the target.
  & cl.exe @flags /Fe:instance-policy-probe.exe "$repo\driver\umd\d3d12\instance-policy-probe.cpp" /link dxgi.lib
  if($LASTEXITCODE){throw 'Instance policy probe build failed'}
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
  # BD-075: the shared surface's two records as bytes on the wire. The contract header is the one reader and
  # writer for the kernel driver, both shells and the Mesa winsys, so its test takes no API header at all.
  & cl.exe @flags /analyze /analyze:external- /Fe:shared-surface-test.exe "$repo\driver\contract\test\shared-surface-test.cpp"
  if($LASTEXITCODE){throw 'Shared surface contract test build failed'}
  & .\shared-surface-test.exe
  if($LASTEXITCODE){throw 'Shared surface contract tests failed'}
  & cl.exe @flags /Fe:allocation-request-test.exe "$repo\driver\umd\d3d12\allocation-request-test.cpp" "$repo\driver\kmd\umd_blob.c"
  if($LASTEXITCODE){throw 'Allocation request test build failed'}
  & .\allocation-request-test.exe
  if($LASTEXITCODE){throw 'Allocation request tests failed'}
  & cl.exe @flags /Fe:paging-test.exe "$repo\driver\umd\d3d12\paging-test.cpp"
  if($LASTEXITCODE){throw 'Paging test build failed'}
  & .\paging-test.exe
  if($LASTEXITCODE){throw 'Paging tests failed'}
  & cl.exe @flags /Fe:ddi-experiment-test.exe "$repo\driver\umd\d3d12\ddi-experiment-test.cpp"
  if($LASTEXITCODE){throw 'Experiment source test build failed'}
  & .\ddi-experiment-test.exe
  if($LASTEXITCODE){throw 'Experiment source tests failed'}
  & cl.exe @flags /Fe:replay-log-test.exe "$repo\driver\umd\d3d12\replay-log-test.cpp"
  if($LASTEXITCODE){throw 'Replay log test build failed'}
  & .\replay-log-test.exe
  if($LASTEXITCODE){throw 'Replay log tests failed'}
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
  & cl.exe @flags /Fe:hosted-instance-test.exe "$repo\driver\umd\d3d12\hosted-instance-test.cpp"
  if($LASTEXITCODE){throw 'Hosted instance test build failed'}
  & .\hosted-instance-test.exe
  if($LASTEXITCODE){throw 'Hosted instance tests failed'}
  & cl.exe @flags /Fe:hosted-dispatch-test.exe "$repo\driver\umd\d3d12\hosted-dispatch-test.cpp" "$repo\driver\umd\d3d12\hosted-dispatch.cpp"
  if($LASTEXITCODE){throw 'Hosted dispatch test build failed'}
  & .\hosted-dispatch-test.exe
  if($LASTEXITCODE){throw 'Hosted dispatch tests failed'}
  & cl.exe @flags /Fe:hosted-sparse-test.exe "$repo\driver\umd\d3d12\hosted-sparse-test.cpp" "$repo\driver\umd\d3d12\hosted-dispatch.cpp"
  if($LASTEXITCODE){throw 'Hosted sparse test build failed'}
  & .\hosted-sparse-test.exe
  if($LASTEXITCODE){throw 'Hosted sparse tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:queue-engine-test.exe "$repo\driver\umd\d3d12\queue-engine-test.cpp" "$repo\driver\umd\d3d12\queue-engine.cpp" $engineLib
  if($LASTEXITCODE){throw 'Queue engine test build failed'}
  & .\queue-engine-test.exe
  if($LASTEXITCODE){throw 'Queue engine tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:hosted-queue-test.exe "$repo\driver\umd\d3d12\hosted-queue-test.cpp" "$repo\driver\umd\d3d12\hosted-queue.cpp" "$repo\driver\umd\d3d12\queue-engine.cpp" $engineLib
  if($LASTEXITCODE){throw 'Hosted queue test build failed'}
  & .\hosted-queue-test.exe
  if($LASTEXITCODE){throw 'Hosted queue tests failed'}
  # One device, several DDI threads at once: scopes, hosted callbacks and heap imports (no GPU).
  & cl.exe @flags /DAMDGPU_WDDM_D3D12_HOST_TEST /Fe:entry-concurrency-test.exe "$repo\driver\umd\d3d12\entry-concurrency-test.cpp" "$repo\driver\umd\d3d12\device-engine.cpp" "$repo\driver\umd\d3d12\adapter-caps.cpp" "$repo\driver\umd\d3d12\hosted-dispatch.cpp" "$repo\driver\umd\d3d12\hosted-queue.cpp" "$repo\driver\umd\d3d12\queue-engine.cpp" "$repo\driver\umd\d3d12\heap-import.cpp" $engineLib
  if($LASTEXITCODE){throw 'Entry concurrency test build failed'}
  & .\entry-concurrency-test.exe
  if($LASTEXITCODE){throw 'Entry concurrency tests failed'}
  & cl.exe @flags /Fe:heap-import-test.exe "$repo\driver\umd\d3d12\heap-import.cpp" "$repo\driver\umd\d3d12\heap-import-test.cpp"
  if($LASTEXITCODE){throw 'heap-import test build failed'}
  & .\heap-import-test.exe
  if($LASTEXITCODE){throw 'heap-import tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:native-queue-ddi-test.exe "$repo\driver\umd\d3d12\native-queue-ddi.cpp" "$repo\driver\umd\d3d12\native-queue-ddi-test.cpp" "$repo\driver\umd\d3d12\queue-engine.cpp" $engineLib
  if($LASTEXITCODE){throw 'native-queue-ddi test build failed'}
  & .\native-queue-ddi-test.exe
  if($LASTEXITCODE){throw 'native-queue-ddi tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:native-residency-ddi-test.exe "$repo\driver\umd\d3d12\native-residency-ddi.cpp" "$repo\driver\umd\d3d12\native-residency-ddi-test.cpp"
  if($LASTEXITCODE){throw 'native-residency-ddi test build failed'}
  & .\native-residency-ddi-test.exe
  if($LASTEXITCODE){throw 'native-residency-ddi tests failed'}
  & cl.exe @flags /Fe:ddi-entry-test.exe "$repo\driver\umd\d3d12\ddi-entry-test.cpp"
  if($LASTEXITCODE){throw 'DDI entry scope test build failed'}
  & .\ddi-entry-test.exe
  if($LASTEXITCODE){throw 'DDI entry scope tests failed'}
  & cl.exe @flags "/external:I$MesaSource\src\util" "/external:I$VulkanInclude" "/external:I$EngineInclude" /Fe:adapter-caps-probe.exe "$repo\driver\umd\d3d12\adapter-caps-probe.cpp" /link dxgi.lib
  if($LASTEXITCODE){throw 'Adapter caps probe build failed'}
  & .\adapter-caps-probe.exe --help
  if($LASTEXITCODE){throw 'Adapter caps probe help failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:device-table-test.exe "$repo\driver\umd\d3d12\device-table-test.cpp" $engineLib
  if($LASTEXITCODE){throw 'Device table composition test build failed'}
  & .\device-table-test.exe
  if($LASTEXITCODE){throw 'Device table composition tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:present-outputs-test.exe "$repo\driver\umd\d3d12\present-outputs-test.cpp"
  if($LASTEXITCODE){throw 'Present outputs test build failed'}
  & .\present-outputs-test.exe
  if($LASTEXITCODE){throw 'Present outputs tests failed'}
  # The per-application MaxFrameLatency gate of Present: the ring, the poll loop and the counters.
  & cl.exe @flags /analyze /analyze:external- /Fe:frame-latency-test.exe "$repo\driver\umd\d3d12\frame-latency-test.cpp"
  if($LASTEXITCODE){throw 'Frame latency test build failed'}
  & .\frame-latency-test.exe
  if($LASTEXITCODE){throw 'Frame latency tests failed'}
  & cl.exe @flags /analyze /analyze:external- /Fe:shell-core-ddi-test.exe "$repo\driver\umd\d3d12\shell-core-ddi-test.cpp"
  if($LASTEXITCODE){throw 'Shell core DDI test build failed'}
  & .\shell-core-ddi-test.exe
  if($LASTEXITCODE){throw 'Shell core DDI tests failed'}
  & cl.exe @flags /Fe:adapter-kmt-probe.exe "$repo\driver\umd\d3d12\adapter-kmt-probe.cpp" /link dxgi.lib gdi32.lib
  if($LASTEXITCODE){throw 'Adapter KMT probe build failed'}
  & .\adapter-kmt-probe.exe --help
  if($LASTEXITCODE){throw 'Adapter KMT probe help failed'}
 } finally {Pop-Location}
} finally {Restore-ProcessEnvironment $saved}
Write-Host 'Adapter caps linked; functional device tables and deployment remain separate gates.'
