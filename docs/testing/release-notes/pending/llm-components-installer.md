# Pending release-notes lines: the extras of the installer (llama.cpp, Ollama, models)

This file is not a release. It holds the tester-facing lines for the next tester release notes. The release step
copies the lines into `docs/testing/release-notes/<version>-tester.N.md` and removes this file. The Ollama half
of this work has its own pending file, `llm-components-ollama.md`. A release that holds one half without the
other keeps only that half's lines.

## New

- **The installer can install the programs that run language models on the board.** It asks one question for
  each one, and `yes` is the answer in capitals, so Enter is enough. The two are our build of llama.cpp and our
  build of Ollama. Both are open projects under the MIT licence. They go into
  `C:\Program Files\amdgpu-wddm\tools\llama` and `C:\Program Files\amdgpu-wddm\tools\ollama`, and both folders
  go on the machine search path, so you can start them in any command prompt after the restart.

- **The installer can also get a model for you.** After the two questions it shows a short list with the size
  and the licence of each model: Gemma 12B, which is the model this board was measured with, Qwen3 1.7B, which
  is small and quick, and Qwen3.6 35B A3B, which needs the board memory at 12 GB. The answer is none, because a
  model is a download of gigabytes. The model files go into
  `C:\Users\Public\Documents\amdgpu-wddm\models`, where you can find them in File Explorer. For Ollama the
  installer gets the same model through Ollama itself, which keeps it in its own format in its own folder: a
  model chosen while both programs are installed is therefore downloaded twice and kept twice, about 13 GB for
  Gemma 12B. The question says so for each model, with the sum, before anything is downloaded.

- **An extra and a model are checked before they are used.** The package holds the address, the size and the
  SHA-256 of every download. A file that does not match its SHA-256 is refused. An extra for which the package
  holds no SHA-256 is refused as well, with a line that says so. A refused, a skipped or a failed extra never
  stops the driver installation: the driver is installed first, and the extras come last.

- **An installation without a window can choose with switches.** `-Components llama,ollama` and
  `-NoComponents`, `-Models gemma12b` and `-NoModels`, and `-ComponentDir <folder>` to take the files from a
  folder instead of the internet. `prepare-offline.cmd` takes the same switches and puts the files into
  `components\` and `models\` of the prepared folder, next to the firmware. An installation from that folder
  needs no internet for them. `-ComponentDir` also takes a folder that holds the zips and the model files
  together, and anything that is not in the folder is downloaded instead, with a line that says which file.

- **The uninstaller removes every registration of an extra and keeps your models.** It removes the two folders
  with the install root, the two entries in the machine search path, and the logon entry and settings that an
  extra wrote for itself. Each extra removes its own registration first, with its own script. The models stay,
  because they are your files. `uninstall.cmd -RemoveModels` removes them as well, from both model folders: the
  GGUF folder and the folder in which Ollama keeps its own format.
  The list at the end of the uninstall shows both, so you can see what is still on the computer.

## Known behaviour

- The questions are asked once, in the first run of the installation. The run after the restart has no window,
  so it reads your answers from the state of the installation. A second run of `install.cmd` over a finished
  installation asks again.

- An extra writes its own per-user settings for the account that started `install.cmd`. Another account on the
  same computer finds the programs on the search path, and starts the Ollama server itself.

- `C:\Program Files\amdgpu-wddm\licenses` holds the licence text of the driver and its own programs. Each extra
  carries its own licence inside its folder, `tools\llama` and `tools\ollama`, and the installer prints the
  licence and its address for every extra and every model before it downloads anything. The page
  `docs/testing/llm-components.md` holds the same addresses.

- Neither extra puts anything in the Start menu. Both are started from a command prompt, and their folders are on
  the machine search path after the restart that ends the installation.
