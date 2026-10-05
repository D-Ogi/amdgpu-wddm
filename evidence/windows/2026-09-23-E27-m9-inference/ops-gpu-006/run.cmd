@echo off
set VK_DRIVER_FILES=C:\BC250\m8\radeon_icd.json
set VK_ICD_FILENAMES=%VK_DRIVER_FILES%
set MESA_SHADER_CACHE_DISABLE=true
set BC250_IB_DWORDS=
set PATH=C:\BC250\m8;%PATH%
set GGML_VK_DISABLE_F16=
set GGML_VK_DISABLE_GRAPH_OPTIMIZE=
set GGML_VK_DISABLE_FUSION=
set GGML_VK_DISABLE_ASYNC=
set GGML_VK_DISABLE_HOST_VISIBLE_VIDMEM=
mkdir C:\BC250\m9\ops-gpu-006\cpu
mkdir C:\BC250\m9\ops-gpu-006\gpu
set BC250_TENSOR_DIR=C:\BC250\m9\ops-gpu-006\cpu
set BC250_KMD_LOG_DIR=
C:\BC250\m9\backend-tests\bc250-node-probe.exe -m C:\BC250\m9\models\stories260K.gguf -p "Once upon a time" -ngl 0 -t 6 < NUL > C:\BC250\m9\ops-gpu-006\cpu.out 2> C:\BC250\m9\ops-gpu-006\cpu.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-006\cpu.exit
set BC250_TENSOR_DIR=C:\BC250\m9\ops-gpu-006\gpu
set BC250_KMD_LOG_DIR=C:\BC250\m9\ops-gpu-006\gpu
C:\BC250\m9\backend-tests\bc250-node-probe.exe -m C:\BC250\m9\models\stories260K.gguf -p "Once upon a time" -ngl 99 -t 6 < NUL > C:\BC250\m9\ops-gpu-006\gpu.out 2> C:\BC250\m9\ops-gpu-006\gpu.err
echo %ERRORLEVEL% > C:\BC250\m9\ops-gpu-006\gpu.exit
exit /b
