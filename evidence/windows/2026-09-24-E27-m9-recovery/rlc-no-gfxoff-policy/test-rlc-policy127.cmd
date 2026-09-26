@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\m9
python P:\bc-250\bc250-win\experiments\E27-m9-inference\generate-rlc-gfxoff-policy-test.py P:\bc-250\bc250-win P:\bc-250\ref\linux-stable-6.18.52-amdgpu\gfx_v10_0.c P:\bc-250\ref\linux-src\drivers\gpu\drm\amd\include\amd_shared.h rlc-policy127.c %*
if errorlevel 1 exit /b 1
cl /nologo /W4 /WX /TC /IP:\bc-250\bc250-win\third_party\linux-amdgpu rlc-policy127.c /Fe:rlc-policy127.exe
if errorlevel 1 exit /b 1
rlc-policy127.exe
