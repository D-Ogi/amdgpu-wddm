@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
C:\msys64\mingw64\bin\cmake.exe -S P:/bc-250/ref/llvm-project-23.1.2/llvm -B P:/bc-250/scratch/llvm2312-build -G Ninja -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_INSTALL_PREFIX=P:/bc-250/toolchain/llvm2312 -DCMAKE_CXX_FLAGS=/utf-8 -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_RUNTIMES=OFF -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_BUILD_TOOLS=OFF -DLLVM_ENABLE_DIA_SDK=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_PARALLEL_LINK_JOBS=2
if errorlevel 1 exit /b 1
C:\msys64\mingw64\bin\ninja.exe -C P:/bc-250/scratch/llvm2312-build -j12 llvm-config install
