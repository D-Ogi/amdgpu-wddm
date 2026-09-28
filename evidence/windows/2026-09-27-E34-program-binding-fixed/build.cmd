@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set LLVM_CONFIG=P:\bc-250\scratch\llvm2312-build\bin\llvm-config.exe
set CMAKE_PREFIX_PATH=P:\bc-250\toolchain\llvm2312
set PATH=P:\bc-250\scratch\glslang\bin;P:\bc-250\scratch\llvm2312-build\bin;C:\msys64\mingw64\bin;P:\bc-250\toolchain\winflexbison-2.5.25;%PATH%
set INCLUDE=P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um;P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared;%INCLUDE%
set PYTHONPATH=P:\bc-250\scratch\py
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
C:\Python314\python.exe -m mesonbuild.mesonmain setup --force-fallback-for=zlib P:\bc-250\scratch\m13-native-zink-build P:\bc-250\scratch\m13-native-zink-src --buildtype=debugoptimized -Dllvm=disabled -Damd-use-llvm=false -Dvulkan-drivers=[] -Dgallium-drivers=zink -Dgallium-d3d10umd=true -Dgallium-d3d10-dll-name=bc250d3d_zink -Ddefault_library=static -Dplatforms=windows -Dvideo-codecs=[] -Degl=disabled -Dglx=disabled -Dzstd=disabled
if errorlevel 1 exit /b 1
C:\msys64\mingw64\bin\ninja.exe -j12 -C P:\bc-250\scratch\m13-native-zink-build src/gallium/targets/d3d10umd/bc250d3d_zink.dll
