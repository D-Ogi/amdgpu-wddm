@echo off
set VK_DRIVER_FILES=C:\BC250\m9\shader-eviction-icd\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
set BC250_TRACE_SUBMITS=0
set BC250_IB_DWORDS=
set BC250_TEST_EVICT_ON_UNMAP=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m9\shader-alias-eviction119\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 3 --alias-intermediate < NUL > C:\BC250\m9\shader-alias-eviction119\baseline.out 2> C:\BC250\m9\shader-alias-eviction119\baseline.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-alias-eviction119\baseline.exit
if not "%probe_exit%"=="0" exit /b 1
C:\BC250\m9\shader-alias-eviction119\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 3 --alias-intermediate --evict-alias < NUL > C:\BC250\m9\shader-alias-eviction119\evict.out 2> C:\BC250\m9\shader-alias-eviction119\evict.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-alias-eviction119\evict.exit
if not "%probe_exit%"=="0" exit /b 1
C:\BC250\m9\shader-alias-eviction119\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 3 --alias-intermediate --evict-alias --stale-input-control < NUL > C:\BC250\m9\shader-alias-eviction119\stale.out 2> C:\BC250\m9\shader-alias-eviction119\stale.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-alias-eviction119\stale.exit
if not "%probe_exit%"=="1" exit /b 1
exit /b 0
