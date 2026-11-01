# Pending release-notes lines: PyTorch of AMD's TheRock packages on our HIP runtime

This file is not a release. It holds the tester-facing lines for the next tester release notes. The release
step copies the lines into the sections of the same name in `docs/testing/release-notes/<version>.md` and
deletes this file. The headings here are the four sections that `build-release.ps1` requires, so that the
lines move across without a new section name.

## New

- **Experimental: PyTorch from AMD's TheRock packages on our HIP runtime.** The package has a new folder,
  `tools\hip\rocm`, for the Python packages `rocm-sdk-core` and `rocm-sdk-libraries` 10.3.0a20261030 of AMD's
  TheRock build for gfx1010. It holds our `amdhip64_7.dll`, our `libhipblaslt.dll` (it adds the 8-bit integer
  matrix product that `torch._int_mm` needs) and the step that puts both into a Python environment (venv). Run it
  once for each venv, with no Python of that venv open:

  ```
  powershell -ExecutionPolicy Bypass -File "C:\Program Files\amdgpu-wddm\tools\hip\rocm\attach-rocm-venv.ps1" -Venv C:\path\to\venv
  ```

  The step keeps AMD's two DLLs in the venv and refuses a venv of another TheRock build. `-Check` shows the state
  of a venv, and `-Detach` puts AMD's DLLs back. The folder is not on PATH, so no other program loads these DLLs.

## Settings affected

- The attach step lists each venv that it changes in `%ProgramData%\amdgpu-wddm\rocm-venvs`. The uninstaller puts
  AMD's DLLs back into each listed venv and then deletes the list. A venv that no longer exists is skipped with a
  line in the uninstall log. If a Python of a listed venv is open during the uninstall, that venv keeps our DLLs:
  the uninstaller shows a warning with the venv's folder, and `pip install --force-reinstall rocm-sdk-core
  rocm-sdk-libraries` in that venv puts AMD's DLLs back.

## Known issues

- After an update of the driver, a venv keeps the DLLs of the version before until you run the attach step for it
  again. `-Check` then says that an earlier payload is attached.
