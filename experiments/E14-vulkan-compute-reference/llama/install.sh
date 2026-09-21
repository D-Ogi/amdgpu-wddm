#!/bin/sh
# E14, on the probe (Alpine 3.24 from the diagnostic stick, running from RAM, amdgpu loaded): a Vulkan user-mode stack
# and llama.cpp with its Vulkan backend, installed into RAM from Alpine's online repositories. The stick is not written:
# the repositories are named on the command line, /etc/apk/repositories stays as it is, nothing is committed with lbu.
# Needs a route to the internet. About 680 MiB of RAM after everything is in.
set -u
R="--repository https://dl-cdn.alpinelinux.org/alpine/v3.24/main --repository https://dl-cdn.alpinelinux.org/alpine/v3.24/community"
apk add $R mesa-vulkan-ati vulkan-tools vulkan-loader-dev vulkan-headers glslang gcc musl-dev \
	llama.cpp llama.cpp-vulkan llama.cpp-cpu llama.cpp-extras 2>&1 | tail -1
apk info -v mesa-vulkan-ati llama.cpp vulkan-loader glslang gcc 2>/dev/null
mkdir -p /tmp/e14/models /tmp/e14/llama
vulkaninfo --summary > /tmp/e14/vulkaninfo-summary.txt 2>&1
vulkaninfo > /tmp/e14/vulkaninfo.txt 2>&1
grep -E 'deviceName|driverInfo|apiVersion' /tmp/e14/vulkaninfo-summary.txt
# Models: two of llama.cpp's own CI models and one model of a useful size. Public files, fetched into RAM, hashed.
cd /tmp/e14/models || exit 1
fetch() { [ -s "$2" ] || wget -q -O "$2" "$1/$2"; sha256sum "$2"; }
fetch https://huggingface.co/ggml-org/models/resolve/main/tinyllamas stories260K.gguf
fetch https://huggingface.co/ggml-org/models/resolve/main/tinyllamas stories15M-q4_0.gguf
fetch https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main tinyllama-1.1b-chat-v1.0.Q4_0.gguf
