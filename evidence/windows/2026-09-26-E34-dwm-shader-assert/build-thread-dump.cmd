@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\BC-250\scratch\tmp
set TMP=P:\BC-250\scratch\tmp
cd /d P:\BC-250\scratch\g0-hosted
cl /nologo /DUNICODE /D_UNICODE /O2 /MT /W4 /WX thread-dump.cpp /Fe:thread-dump.exe /link dbghelp.lib advapi32.lib