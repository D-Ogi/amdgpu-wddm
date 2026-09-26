@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%TEMP%
if not exist %BC250_ROOT%\scratch\m9\shader-coherency mkdir %BC250_ROOT%\scratch\m9\shader-coherency
cd /d %BC250_ROOT%\scratch\m9\shader-coherency
rem C4702: inherited E14 fallback return after fatal exit.
cl /wd4702 /nologo /W4 /WX /O2 /std:c11 /I %BC250_ROOT%\scratch\mesa-wddm2\include %BC250_ROOT%\bc250-win\experiments\E27-m9-inference\shader-coherency-probe.c %BC250_ROOT%\scratch\vkcompute\vulkan-1.lib /Fe:shader-coherency-probe.exe
if errorlevel 1 exit /b 1
shader-coherency-probe.exe --help
