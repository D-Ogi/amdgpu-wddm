# compute/ollama - Ollama for this driver

Ollama is the second large-language-model component of a tester release. The first one is our
llama.cpp build, which is the measuring instrument. Ollama is what a person actually uses, because
it holds the models, the chat template and the HTTP interface in one command.

This directory holds the build, the patches and the runtime entry point. It holds no Ollama source
and no binaries: the source is cloned, and the artifacts go to `scratch`.

## What comes out

One zip, `bc250-ollama-<version>-bc250.<train>.zip`, with one directory inside it:

```
ollama\
  ollama.exe                     the command and the server in one binary
  bc250-ollama.ps1               start, stop, status, pull, register, unregister
  README.md LICENSE THIRD-PARTY.md
  lib\ollama\
    llama-server.exe             the runner, and the processor backend next to it
    ggml-base.dll ggml.dll llama.dll ggml-cpu-*.dll
    vulkan\ggml-vulkan.dll       the Vulkan backend
    rocm_bc250\ggml-hip.dll      the HIP backend, compiled for gfx1013
    rocm_bc250\amdhip64.dll      our HIP runtime
    rocm_bc250\bc250hipblas.dll  our BLAS entry points
```

Ollama looks for backends in `<directory of ollama.exe>\lib\ollama\*`, so the layout is the whole
of the wiring. `ollama.exe --version` carries `-bc250.<train>`, so a support log says at once
whether the user runs our build.

## The two backends, and which one runs

Both backends reach the same board through different paths:

| Backend | Directory | Path to the hardware |
|---|---|---|
| Vulkan | `vulkan` | the system Vulkan driver of this project |
| HIP | `rocm_bc250` | our HIP runtime on our kernel driver, `compute/hip` |

Today Vulkan generates tokens faster than our HIP on this board. Measured on Gemma 12B:
25.25 ms per token on Vulkan against 31.73 ms on HIP. The gap is not settled, both are being
worked on, and the choice is therefore one setting rather than a compiled-in decision:

```
HKCU\Environment  BC250_OLLAMA_BACKEND = auto | vulkan | rocm_bc250
```

`bc250-ollama.ps1` turns the value into `OLLAMA_LLM_LIBRARY` for the server process, which is
Ollama's own switch for limiting discovery to one backend directory. `auto` sets nothing and lets
Ollama choose. Ollama prefers HIP over Vulkan for the same device, so `auto` means HIP today. The
installer writes `auto`, and the lab measurement decides what the default becomes.

The server process also gets two settings it needs on every start:

- `OLLAMA_IGPU_ENABLE=1`, because this board's GPU is the machine's only GPU and reports itself
  integrated.
- `OLLAMA_FLASH_ATTENTION=1`, because both of our backends are built with the flash attention
  kernels and the project runs them on.

## The patches

Ollama is MIT. See `PROVENANCE.md`.

`patches/ollama/001-bc250-integrated-gpu-discovery.patch` changes two discovery defaults. Both
exist to keep a weak integrated GPU out of the way of a discrete one, which is the opposite of
this board's situation:

1. `gfx1013` is added to the list of AMD targets admitted by default. The list exists for this
   case and named one part. The omission is a missing entry, not a hardware limit: the same class
   of omission has appeared in LLVM, in Mesa and in llama.cpp target lists for this chip.
2. A device the list does not name is dropped. A Vulkan device has no gfx target to match on, so
   the board was dropped from the Vulkan backend as well and the processor was all that remained.
   Our build admits integrated GPUs by default instead. `OLLAMA_IGPU_ENABLE=0` still turns them
   off, because the branch that reads the variable answers first.

`patches/ollama/002-hip-install-without-rocm-payload.patch` lets a HIP build stand on a runtime
that is not ROCm. Upstream installs ROCm's own payload next to the backend module, its BLAS
libraries, its code-object manager and its Tensile kernel files, and stops the configure when the
code-object manager is absent. Our runtime has none of those: it brings `amdhip64.dll` and
`bc250hipblas.dll`, it loads code objects itself, and it has no Tensile files. The patch gates the
ROCm payload on the same condition that the top of the same file already uses to select ROCm's
compiler flags, a runner directory named `rocm_v*`, so every upstream build is unchanged.

`patches/ollama/003-keep-bundled-non-rocm-hip-runtime.patch` stops a system HIP runtime from
taking the place of ours. AMD's Windows driver also puts ROCm runtime libraries in the system
directory, and the runner's library path is reordered to prefer whichever `amdhip64.dll` has the
higher file version. Between two copies of ROCm's runtime that is right. For ours it is wrong: a
higher-versioned file of the same name in the system directory is a different product, not a newer
copy of a runtime that talks to our kernel driver. The same `rocm_v*` condition decides. Without
the patch the protection is accidental, because our runtime carries no version resource and the
comparison cannot decide: adding one later would silently switch the runtime on any machine that
also has AMD's driver.

The llama.cpp side needs no patch of its own here. Ollama builds its runner from a pinned
llama.cpp, and `build-ollama.ps1` places our two measured llama.cpp patches where Ollama's own
patch applier finds them:

| Patch | What it is for |
|---|---|
| `compute/hip/patches/llama-no-device-printf.patch` | the device-side `printf` in the HIP arch check, which this runtime does not carry |
| `compute/hip/patches/llama-bc250-allocation-cap.patch` | the measured per-allocation limit of this runtime, defect BD-117 |

The files are not copied into this directory. One copy of each patch exists, in
`compute/hip/patches`, and the HIP backend of Ollama and our llama.cpp asset read the same file.
`build-ollama.ps1 -Stage prepare` records the SHA-256 of every patch it used in
`inputs.json`, next to the Ollama commit, the llama.cpp pin and the compiler version.

## Building

```powershell
# the whole thing
powershell -NoProfile -ExecutionPolicy Bypass -File compute\ollama\build-ollama.ps1 -Stage all

# or one stage at a time
...\build-ollama.ps1 -Stage prepare   # toolchains, patches, inputs.json
...\build-ollama.ps1 -Stage base      # llama-server and the processor backend
...\build-ollama.ps1 -Stage vulkan    # ggml-vulkan
...\build-ollama.ps1 -Stage hip       # ggml-hip for gfx1013
...\build-ollama.ps1 -Stage go        # ollama.exe
```

What the build needs, all of it portable and none of it on drive C: except the Visual Studio that
supplies the Windows SDK headers:

| Tool | Where |
|---|---|
| Go | `toolchain/go-1.27.2` |
| AMD clang 22.1.8 | `toolchain/llvm-amdgpu-22.1.8` |
| CMake, Ninja | `C:/msys64/mingw64/bin` |
| glslc, the Vulkan headers and loader | `toolchain/vk-build/Library` |
| our ROCm-shaped root, with `amdhip64.dll` and `bc250hipblas.dll` | `-RocmRoot`, by default the root of the BD-117 consumer build |

One compiler builds every stage: AMD's clang, targeting `x86_64-pc-windows-msvc`. That is the
compiler our llama.cpp release asset uses, so the base, the Vulkan backend and the HIP backend
share an ABI and a set of optimisation options.

Two build facts worth keeping:

- The HIP runner directory is `rocm_bc250`, not `rocm_v*`. Upstream gates two sets of ROCm
  compiler flags on a directory name that starts with `rocm_v`, and those flags belong to ROCm's
  compiler, not to ours.
- llama.cpp's processor kernels prefetch a block pointer through `_mm_prefetch`, whose parameter
  is `const char *`. The Microsoft compiler accepts it. Clang has made that class of C warning an
  error by default since version 20. The build keeps those four diagnostics as warnings rather
  than editing upstream sources.

## Packaging and the host checks

```powershell
...\compute\ollama\host-checks.ps1          # four checks, nothing left running
...\compute\ollama\package-ollama.ps1       # the zip, its SHA-256 and a JSON record
```

`host-checks.ps1` runs what a machine without a BC-250 can answer: the two binaries print their
versions, the server starts on a free port with the GPU directories skipped, answers its own API
and is stopped by the same script, and every import of every binary in the tree resolves.
`check-imports.ps1` is that last check on its own. `package-ollama.ps1` runs it before it makes
the zip, because a zip that misses one DLL fails on a tester's machine with a dialog and no log
line.

Whether the two GPU backends load is a lab question. The lab plan of this work states the runs.

## A stock Ollama on the same machine

Our component never writes to an official Ollama install, never stops its service and never takes
its port. `bc250-ollama.ps1 status` reports `foreign: true` when something that we did not start
listens on our port, `start` then refuses with exit code 2, and `register` picks the next port when
11434 is already busy at install time. Our models live in
`%LOCALAPPDATA%\bc250-ollama\models`, which only our server process is told about, so a stock
install keeps its own store.

`OLLAMA_MODELS` is therefore set per process and written to no registry key. The release carries
two model stores, one per component: the llama.cpp component keeps GGUF files where the installer
puts them, and this component keeps Ollama's own format in the path above. Ollama reads only
`OLLAMA_MODELS` and only its own format, so one shared directory is not available. Both paths are
in the installer's report.

## What the installer calls

| Command | What it does | Exit code |
|---|---|---|
| `bc250-ollama.ps1 status` | one JSON object: `running`, `port`, `pid`, `backend`, `foreign` | 0 |
| `bc250-ollama.ps1 start` | starts the server in the user's session, detached, no window | 0 running, 2 the port is held by a stock Ollama, 1 other failure |
| `bc250-ollama.ps1 stop` | stops only a server this script started | 0 |
| `bc250-ollama.ps1 pull -Model <tag>` | pulls a registry tag through our server | 0 pulled, 1 failure |
| `bc250-ollama.ps1 register` | writes the logon task, the backend setting and the port | 0 |
| `bc250-ollama.ps1 unregister` | removes the logon task and the two settings, and nothing else | 0 |

The installer reads the exit code and the JSON. It parses no prose and no Windows message, so the
language of the system changes nothing (issue #35). `register` rewrites an existing logon task
rather than adding a second one, so a repair can run twice.
