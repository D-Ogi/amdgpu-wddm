param([Parameter(Mandatory=$true)][string]$Root,[Parameter(Mandatory=$true)][string]$Out,
 [ValidateSet('none','nolock')][string]$Mutation='none')
# BD-289: the flip target mapping rule (scanout_map.h) under concurrency, and the placement of the scan-out mutex
# in the driver source. Mutation 'nolock' runs the model without the mutex; it must show faults.
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
# Source placement: the blit takes the mutex around WddmPresentBlitOwned; the mapping asserts it; the release takes it.
$blit=Body 'wddm.c' 'static void WddmPresentBlit(_In_'
$lock=$blit.IndexOf('DcnScanoutLock(');$call=$blit.IndexOf('WddmPresentBlitOwned(');$unlock=$blit.IndexOf('DcnScanoutUnlock(')
if($lock -lt 0 -or $call -lt $lock -or $unlock -lt $call){throw 'WddmPresentBlit: WddmPresentBlitOwned is not inside DcnScanoutLock/DcnScanoutUnlock'}
$map=Body 'dcn.c' 'BOOLEAN DcnScanoutMapping('
if($map.IndexOf('DcnScanoutLockHeld(') -lt 0 -or $map.IndexOf('ScanoutMapSelect(') -lt 0){throw 'DcnScanoutMapping: no lock assertion or no ScanoutMapSelect'}
$rel=Body 'dcn.c' 'void DcnUnmapScanout('
$a=$rel.IndexOf('DcnScanoutLock(');$b=$rel.IndexOf('ScanoutMapRelease(');$c=$rel.IndexOf('DcnScanoutUnlock(')
if($a -lt 0 -or $b -lt $a -or $c -lt $b){throw 'DcnUnmapScanout: ScanoutMapRelease is not inside the mutex'}
if((Get-Content -Raw "$repo/driver/kmd/pnp.c").IndexOf('KeInitializeMutex(&device->DcnScanoutLock') -lt 0){throw 'pnp.c: DcnScanoutLock is not initialized'}
# The model, compiled with clang-cl over the same header.
$clang="$Root\toolchain\llvm-23.1.4-win64\bin\clang-cl.exe"
if(-not (Test-Path $clang)){throw "Missing $clang"}
$vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$msvc=Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name | Select-Object -Last 1
$sdk="$Root\toolchain\nuget\microsoft.windows.sdk.cpp\c";$libs="$Root\toolchain\nuget\microsoft.windows.sdk.cpp.x64\c"
& $clang --version | Select-Object -First 1 | Tee-Object "$Out/compiler.txt"
# The SDK and MSVC headers are system headers (/imsvc): /W4 /WX applies to the test and scanout_map.h only.
& $clang /nologo /TC /W4 /WX /O2 /MT -fuse-ld=lld "/I$repo\driver\kmd" "/imsvc$($msvc.FullName)\include" "/imsvc$sdk\Include\10.0.26100.0\ucrt" "/imsvc$sdk\Include\10.0.26100.0\um" "/imsvc$sdk\Include\10.0.26100.0\shared" "/Fo$Out\" "/Fe$Out\scanout_lock_test.exe" "$PSScriptRoot\scanout_lock_test.c" /link "/LIBPATH:$($msvc.FullName)\lib\x64" "/LIBPATH:$libs\ucrt\x64" "/LIBPATH:$libs\um\x64" kernel32.lib
if($LASTEXITCODE -ne 0){throw 'Scanout lock test compile failed'}
$arg=@();if($Mutation -eq 'nolock'){$arg=@('--no-lock')}
$lines=@(& "$Out/scanout_lock_test.exe" @arg);$code=$LASTEXITCODE;$lines | Tee-Object "$Out/test.log"
$hashes=@{};foreach($path in @('driver/kmd/scanout_map.h','driver/kmd/dcn.c','driver/kmd/wddm.c','driver/kmd/test/scanout_lock_test.c')){$hashes[$path]=(Get-FileHash "$repo/$path").Hash}
@{Sources=$hashes;Mutation=$Mutation;ExitCode=$code;Output=$lines} | ConvertTo-Json -Depth 4 | Set-Content "$Out/record.json"
exit $code
