@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set LLVM_CONFIG=%BC250_ROOT%\scratch\llvm2312-build\bin\llvm-config.exe
set CMAKE_PREFIX_PATH=%BC250_ROOT%\toolchain\llvm2312
set PATH=%BC250_ROOT%\scratch\glslang\bin;%BC250_ROOT%\scratch\llvm2312-build\bin;C:\msys64\mingw64\bin;%BC250_ROOT%\toolchain\winflexbison-2.5.25;%PATH%
set INCLUDE=%BC250_ROOT%\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um;%BC250_ROOT%\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared;%INCLUDE%
set PYTHONPATH=%BC250_ROOT%\scratch\py
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%BC250_ROOT%\scratch\tmp
C:\Python314\python.exe -m mesonbuild.mesonmain setup --force-fallback-for=zlib %BC250_ROOT%\scratch\mesa-radv-main-build %BC250_ROOT%\scratch\mesa-radv-main-20260924 --buildtype=debugoptimized -Dllvm=disabled -Damd-use-llvm=false -Dvulkan-drivers=amd -Dgallium-drivers=[] -Ddefault_library=static -Dplatforms=windows -Dvideo-codecs=[] -Degl=disabled -Dglx=disabled -Dzstd=disabled
if errorlevel 1 exit /b 1
C:\msys64\mingw64\bin\ninja.exe -j12 -C %BC250_ROOT%\scratch\mesa-radv-main-build src/amd/vulkan/vulkan_radeon.dll
