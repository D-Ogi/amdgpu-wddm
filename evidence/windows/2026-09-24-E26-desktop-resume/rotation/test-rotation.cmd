@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /nologo /MT /EHsc /std:c++17 scratch\m13\rotation-test.cpp /Fe:scratch\m13\rotation-test.exe /Fo:scratch\m13\rotation-test.obj
if errorlevel 1 exit /b 1
scratch\m13\rotation-test.exe
if errorlevel 1 exit /b 1
cl /nologo /MT /EHsc /std:c++17 scratch\m13\rotation-negative.cpp /Fe:scratch\m13\rotation-negative.exe /Fo:scratch\m13\rotation-negative.obj
if errorlevel 1 exit /b 1
scratch\m13\rotation-negative.exe
if errorlevel 1 exit /b 0
exit /b 2
