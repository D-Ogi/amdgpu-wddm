@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
if not exist P:\bc-250\scratch\m9\shader-coherency mkdir P:\bc-250\scratch\m9\shader-coherency
cd /d P:\bc-250\scratch\m9\shader-coherency
rem C4702: inherited E14 fallback return after fatal exit.
cl /wd4702 /nologo /W4 /WX /O2 /std:c11 /I P:\bc-250\scratch\mesa-wddm2\include P:\bc-250\bc250-win\experiments\E27-m9-inference\shader-coherency-probe.c P:\bc-250\scratch\vkcompute\vulkan-1.lib /Fe:shader-coherency-probe.exe
if errorlevel 1 exit /b 1
shader-coherency-probe.exe --help
