@echo off
set VK_DRIVER_FILES=C:\BC250\m9\quiet-submit\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
set BC250_TRACE_SUBMITS=0
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 2 < NUL > C:\BC250\m9\shader-memory-types0798\type2-positive.out 2> C:\BC250\m9\shader-memory-types0798\type2-positive.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type2-positive.exit
if not "%probe_exit%"=="0" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 2 --stale-input-control < NUL > C:\BC250\m9\shader-memory-types0798\type2-stale.out 2> C:\BC250\m9\shader-memory-types0798\type2-stale.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type2-stale.exit
if not "%probe_exit%"=="1" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 2 < NUL > C:\BC250\m9\shader-memory-types0798\type2-repeat.out 2> C:\BC250\m9\shader-memory-types0798\type2-repeat.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type2-repeat.exit
if not "%probe_exit%"=="0" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 3 < NUL > C:\BC250\m9\shader-memory-types0798\type3-positive.out 2> C:\BC250\m9\shader-memory-types0798\type3-positive.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type3-positive.exit
if not "%probe_exit%"=="0" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 3 --stale-input-control < NUL > C:\BC250\m9\shader-memory-types0798\type3-stale.out 2> C:\BC250\m9\shader-memory-types0798\type3-stale.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type3-stale.exit
if not "%probe_exit%"=="1" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 3 < NUL > C:\BC250\m9\shader-memory-types0798\type3-repeat.out 2> C:\BC250\m9\shader-memory-types0798\type3-repeat.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type3-repeat.exit
if not "%probe_exit%"=="0" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 5 < NUL > C:\BC250\m9\shader-memory-types0798\type5-positive.out 2> C:\BC250\m9\shader-memory-types0798\type5-positive.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type5-positive.exit
if not "%probe_exit%"=="0" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 5 --stale-input-control < NUL > C:\BC250\m9\shader-memory-types0798\type5-stale.out 2> C:\BC250\m9\shader-memory-types0798\type5-stale.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type5-stale.exit
if not "%probe_exit%"=="1" exit /b 1
C:\BC250\m9\shader-memory-types0798\shader-coherency-probe.exe C:\BC250\m8\spv --memory-type 5 < NUL > C:\BC250\m9\shader-memory-types0798\type5-repeat.out 2> C:\BC250\m9\shader-memory-types0798\type5-repeat.err
set probe_exit=%ERRORLEVEL%
echo %probe_exit% > C:\BC250\m9\shader-memory-types0798\type5-repeat.exit
if not "%probe_exit%"=="0" exit /b 1
exit /b 0

