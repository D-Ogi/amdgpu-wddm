{{NAME}}

llama.cpp for the AMD BC-250 (gfx1013) on Windows, with the HIP backend of the amdgpu-wddm driver.
GPU Flash Attention is built in, and the RDNA1 patches that PROVENANCE.txt names add matrix kernels
for this GPU. The programs: llama-server, llama-cli, llama-completion and llama-bench.

REQUIREMENTS

- amdgpu-wddm {{RELEASE}} installed. Its installer puts the HIP runtime into
  <install directory>\tools\hip (by default C:\Program Files\amdgpu-wddm\tools\hip) and adds that
  folder to the machine PATH. The programs load these runtime files from there:
  {{RUNTIME}}.
  This zip does not hold the runtime. Do not copy a runtime file next to these programs. Windows
  loads an amdhip64.dll in System32, or in a folder that is earlier on PATH, before the one in
  tools\hip.
- The Microsoft Visual C++ 2015-2022 Redistributable (x64), version {{VC_RUNTIME}} or newer. The
  amdgpu-wddm installer installs it when the PC has no runtime or an older one.
- A model in GGUF format.

The amdgpu-wddm installer can install this zip as the extra "llama.cpp for this board". It puts the
programs into <install directory>\tools\llama and that folder on the machine PATH. To install the zip
yourself, extract it into a directory of your choice and run the programs from that directory. Open a
new terminal after an install, so that the terminal reads the new PATH.

SERVER (OpenAI-compatible HTTP API)

  llama-server.exe -m C:\models\model.gguf -ngl 99 -fa on --jinja -c 8192 --host 127.0.0.1 --port 8080

  curl.exe http://127.0.0.1:8080/v1/chat/completions -H "Content-Type: application/json" ^
    -d "{\"messages\":[{\"role\":\"user\",\"content\":\"What is the capital of France?\"}]}"

This build has no web page: the server gives the HTTP API only (/v1/chat/completions, /completion,
/health). Use --host 127.0.0.1 unless other computers must connect.

CHAT IN THE TERMINAL

  llama-cli.exe -m C:\models\model.gguf -ngl 99 -fa on --jinja

One question and one answer, through the chat template of the model:

  llama-completion.exe -m C:\models\model.gguf -ngl 99 -fa on --jinja -cnv -st -p "What is the capital of France?"

Always use --jinja (the chat template of the model) with an instruction model. A raw prompt without the
template can give text that looks correct but does not test the model.

BENCHMARK

  llama-bench.exe -m C:\models\model.gguf -ngl 99 -fa 1 -p 512 -n 128

SWITCHES

The RDNA1 prompt kernels are on by default. Each of these environment variables, set to 0 before a
program starts, puts one part back on the upstream kernels, for a comparison of speed or of results:

  GGML_RDNA1_PKF16=0      all the packed-fp16 matrix kernels of the prompt
  GGML_RDNA1_PKF16_ID=0   the packed-fp16 kernels of the mixture-of-experts layers
  GGML_RDNA1_PKF16_Q8=0   the packed-fp16 kernels of q8_0 weights

In a command prompt: set GGML_RDNA1_PKF16=0

LIMITS

- The largest single GPU allocation is {{CAP_HEX}} bytes ({{CAP_TEXT}}). This build divides model
  buffers at tensor boundaries below that limit. A single tensor larger than the limit cannot load.
- The GPU memory is the VRAM carve-out of the BC-250. With an 8 GiB carve-out, Gemma 4 12B at 4 bits
  (6.5 GiB) loads with -ngl 99. For a larger model, set the board memory to 12 GB in the control
  application, or put fewer layers on the GPU (a smaller -ngl).
- If a long GPU job stops with a driver timeout, check the TdrDelay value in the control
  application (default 10 s).

{{MEASURED}}

Sources, patches, authors and build: PROVENANCE.txt. SHA-256 of every other file: manifest.json.
