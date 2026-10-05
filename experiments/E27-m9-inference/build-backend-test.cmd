@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%BC250_ROOT%\scratch\tmp
C:\msys64\mingw64\bin\cmake.exe -S %BC250_ROOT%\scratch\m9\llama.cpp-b9564 -B %BC250_ROOT%\scratch\m9\build-cpu-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DGGML_VULKAN=OFF -DGGML_BACKEND_DL=ON -DGGML_NATIVE=OFF -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF -DLLAMA_BUILD_TESTS=ON -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF
if errorlevel 1 exit /b 1
C:\msys64\mingw64\bin\cmake.exe --build %BC250_ROOT%\scratch\m9\build-cpu-tests --target test-backend-ops -j 8
