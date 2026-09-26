@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cd /d P:\bc-250\scratch\m13\runtime-probe
cl /nologo /O2 /MT /LD router.cpp /Fe:router.dll
