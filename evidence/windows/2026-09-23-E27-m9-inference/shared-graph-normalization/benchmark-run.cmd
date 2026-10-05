@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
cd /d P:\bc-250\scratch\m9\graph-shared-normalize
cl /nologo /W4 /WX /O2 /std:c11 P:\bc-250\bc250-win\driver\kmd\paging_permutation.c P:\bc-250\bc250-win\driver\kmd\test\paging_graph_normalize_bench.c /Fe:test.exe
if errorlevel 1 exit /b 1
test.exe
