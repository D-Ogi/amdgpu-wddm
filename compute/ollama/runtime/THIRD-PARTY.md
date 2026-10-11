# What this folder carries, and under which licence

| Part | Files | Licence |
|---|---|---|
| Ollama | `ollama.exe` | MIT, in `LICENSE` next to this file |
| llama.cpp | `lib\ollama\llama-server.exe`, `llama.dll`, `ggml*.dll`, `mtmd.dll`, `llama-quantize.exe` | MIT, in `lib\ollama\LLAMA_CPP_LICENSE` |
| cpp-httplib | inside `llama-server.exe` | MIT, in `lib\ollama\CPP_HTTPLIB_LICENSE` |
| the other llama.cpp vendored parts | inside the binaries above | listed in `lib\ollama\LLAMA_CPP_VENDORS_LICENSE` |
| the Microsoft Visual C++ runtime | `msvcp140*.dll`, `vcruntime140*.dll`, `concrt140.dll`, `vccorlib140.dll` | redistributable with the Visual Studio licence |
| this driver's HIP runtime | `lib\ollama\rocm_bc250\amdhip64.dll`, `bc250hipblas.dll` | part of this driver |
| this driver's scripts | `bc250-ollama.ps1` | part of this driver |

The models are not here and are not ours. Each model carries its own terms, which the installer
shows next to the model before it downloads anything. Gemma models are under the Gemma terms of
use, not an open-source licence.
