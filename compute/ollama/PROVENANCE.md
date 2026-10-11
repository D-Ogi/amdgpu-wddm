PROVENANCE: Ollama, MIT.

Upstream: https://github.com/ollama/ollama, commit eab97e9f92b9a25c2d52d2cc6c1b1c99bd9fae21
(describes as v0.40.3, 2026-10-09). The commit pins llama.cpp at tag b11351, which Ollama's own
CMake project fetches and patches.

No Ollama source and no Ollama binary is kept in this repository. `patches/ollama/` holds our
changes as patch files against that commit. `build-ollama.ps1` carries that commit as its `-Commit`
default: it clones the upstream repository when the source directory is absent, checks the commit
out, and refuses to build a checkout that is at another commit, because the version string comes
from `version/version.go` and would not change with it. It then applies the patches and records
every input hash in `inputs.json`.

The llama.cpp patches the HIP backend needs are not duplicated here. They stay in
`compute/hip/patches` (PROVENANCE: llama.cpp, MIT), and the build stages them into the directory
where Ollama's own patch applier reads them.
