@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
cl /nologo /W4 /WX /MT /O2 /LD /TC P:\bc-250\scratch\m9\smu-client-fixture\control-fixture.c /Fe:P:\bc-250\scratch\m9\smu-client-fixture\bc250control.dll /Fo:P:\bc-250\scratch\m9\smu-client-fixture\control-fixture.obj
