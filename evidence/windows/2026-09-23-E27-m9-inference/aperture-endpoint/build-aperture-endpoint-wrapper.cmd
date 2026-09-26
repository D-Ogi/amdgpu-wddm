@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
cd /d P:\bc-250\scratch\m9
cl /nologo /W4 /WX /TC aperture-endpoint-wrapper.c /Fe:aperture-endpoint-wrapper.exe
if errorlevel 1 exit /b 1
aperture-endpoint-wrapper.exe
