param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Out,
 [ValidateSet('none','per-present')][string]$Mutation='none')
# BD-289 second form: the kept mappings of the CPU present blit (blit_map_cache.h), and their use in the driver
# source. Mutation 'per-present' runs the tester.36 shape (map and unmap per present); it must show kind flips.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../../..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$Out=(Resolve-Path $Out).Path;$env:TEMP=$Out;$env:TMP=$Out
function Body([string]$File,[string]$Head) {
 $text=Get-Content -Raw "$repo/driver/kmd/$File"
 $start=$text.IndexOf($Head);if($start -lt 0){throw "Missing $Head in $File"}
 $brace=$text.IndexOf('{',$start);$level=1;$end=$brace+1
 while($level -gt 0 -and $end -lt $text.Length){if($text[$end] -eq '{'){$level++};if($text[$end] -eq '}'){$level--};$end++}
 if($level){throw "Unbalanced $Head"}
 return $text.Substring($start,$end-$start)
}
# Source placement: the blit maps its source and destination only through DcnBlitMap and unmaps nothing itself;
# DcnBlitMap asserts the scan-out mutex; DcnUnmapScanout releases the cache inside the mutex; MmioStart clears it.
$owned=Body 'wddm.c' 'static void WddmPresentBlitOwned(_In_'
if($owned -match 'MmUnmapIoSpace\(\s*(map|destinationMap)\b'){throw 'WddmPresentBlitOwned: unmaps a kept blit mapping'}
if($owned -match 'VramMapCpuRange\(device,physical,'){throw 'WddmPresentBlitOwned: maps its source or destination outside DcnBlitMap'}
if(([regex]::Matches($owned,'DcnBlitMap\(')).Count -ne 2){throw 'WddmPresentBlitOwned: expected two DcnBlitMap calls (source, destination)'}
$end=Body 'wddm.c' 'static void WddmEndBlitSource('
if($end -match 'MmUnmapIoSpace'){throw 'WddmEndBlitSource: unmaps the kept source'}
$get=Body 'dcn.c' 'PVOID DcnBlitMap('
if($get.IndexOf('DcnScanoutLockHeld(') -lt 0 -or $get.IndexOf('BlitMapGet(') -lt 0){throw 'DcnBlitMap: no lock assertion or no BlitMapGet'}
$rel=Body 'dcn.c' 'void DcnUnmapScanout('
$a=$rel.IndexOf('DcnScanoutLock(');$b=$rel.IndexOf('BlitMapRelease(');$c=$rel.IndexOf('DcnScanoutUnlock(')
if($a -lt 0 -or $b -lt $a -or $c -lt $b){throw 'DcnUnmapScanout: BlitMapRelease is not inside the mutex'}
if((Get-Content -Raw "$repo/driver/kmd/mmio.c").IndexOf('RtlZeroMemory(&Device->DcnBlitMaps') -lt 0){throw 'mmio.c: DcnBlitMaps is not cleared at start'}
# The model, compiled with clang-cl over the same header.
$clang="$Root\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe"
if(-not (Test-Path $clang)){throw "Missing $clang"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c";$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& $clang --version | Select-Object -First 1 | Tee-Object "$Out/compiler.txt"
# The SDK and MSVC headers are system headers (/imsvc): /W4 /WX applies to the test and blit_map_cache.h only.
& $clang /nologo /TC /W4 /WX /O2 /MT -fuse-ld=lld "/I$repo\driver\kmd" "/imsvc$($msvc.FullName)\include" "/imsvc$sdk\Include\10.0.26100.0\ucrt" "/imsvc$sdk\Include\10.0.26100.0\um" "/imsvc$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\blit_map_cache_test.exe" "$PSScriptRoot\blit_map_cache_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" kernel32.lib
if($LASTEXITCODE -ne 0){throw 'Blit map cache test compile failed'}
$arg=@();if($Mutation -eq 'per-present'){$arg=@('--per-present')}
$lines=@(& "$Out/blit_map_cache_test.exe" @arg);$code=$LASTEXITCODE;$lines | Tee-Object "$Out/test.log"
$hashes=@{};foreach($path in @('driver/kmd/blit_map_cache.h','driver/kmd/dcn.c','driver/kmd/wddm.c','driver/kmd/mmio.c','driver/kmd/test/blit_map_cache_test.c')){$hashes[$path]=(Get-FileHash "$repo/$path").Hash}
@{Sources=$hashes;Mutation=$Mutation;ExitCode=$code;Output=$lines} | ConvertTo-Json -Depth 4 | Set-Content "$Out/record.json"
exit $code
