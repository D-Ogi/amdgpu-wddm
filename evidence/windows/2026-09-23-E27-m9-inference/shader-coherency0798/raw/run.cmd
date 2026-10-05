@echo off
set VK_DRIVER_FILES=C:\BC250\m9\quiet-submit\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set VK_LOADER_DEBUG=driver
set MESA_SHADER_CACHE_DISABLE=true
set BC250_TRACE_SUBMITS=0
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
C:\BC250\m9\shader-coherency0798\shader-coherency-probe.exe C:\BC250\m8\spv < NUL > C:\BC250\m9\shader-coherency0798\positive.out 2> C:\BC250\m9\shader-coherency0798\positive.err
echo %ERRORLEVEL% > C:\BC250\m9\shader-coherency0798\positive.exit
C:\BC250\m9\shader-coherency0798\shader-coherency-probe.exe C:\BC250\m8\spv --stale-input-control < NUL > C:\BC250\m9\shader-coherency0798\stale.out 2> C:\BC250\m9\shader-coherency0798\stale.err
echo %ERRORLEVEL% > C:\BC250\m9\shader-coherency0798\stale.exit
C:\BC250\m9\shader-coherency0798\shader-coherency-probe.exe C:\BC250\m8\spv < NUL > C:\BC250\m9\shader-coherency0798\repeat.out 2> C:\BC250\m9\shader-coherency0798\repeat.err
echo %ERRORLEVEL% > C:\BC250\m9\shader-coherency0798\repeat.exit
exit /b 0

