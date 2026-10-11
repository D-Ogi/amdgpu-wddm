# The extras of the tester package: llama.cpp, Ollama and the models

The tester package installs the driver. It can also install two extra programs that run language models on the
board, and it can get the model files for them. Both extras are optional. The installer asks one question for
each one. The answer `yes` is the default.

This page tells you what the extras are, where they go, which switches control them, and how to remove them.

## The two extras

| Id | What it is | Licence |
| --- | --- | --- |
| `llama` | Our build of llama.cpp for this board. `llama-server.exe` gives you a chat page in a browser. `llama-cli.exe` and `llama-bench.exe` run in a command prompt. | MIT |
| `ollama` | Our build of Ollama for this board. It holds a model server for you and gets models by name. | MIT |

The two reach the board by different ways. The llama.cpp extra of this release is built for the compute path (HIP)
only. The Ollama extra carries both the compute path and the graphics path (Vulkan) and chooses between them with
one setting, `BC250_OLLAMA_BACKEND`: `auto`, `vulkan` or `rocm_bc250`. `auto` is what the installer leaves, it is an
ordinary user environment variable, and the server reads it when it starts, so stop and start the server after a
change.

Use `bc250-ollama.cmd` in the Ollama folder to talk to our Ollama server, not `ollama.exe`. It is the same program,
with the port and the model folder of this installation already set. A bare `ollama.exe` uses the Ollama defaults,
and on a computer that also has an Ollama of its own it reaches that one instead.

Neither extra is part of the package file. The package holds a list, `installer\components.json`, with the
address, the size and the SHA-256 of each download. The installer gets the file, checks the SHA-256, and only
then opens it. The GPU firmware comes the same way.

A download that does not match its SHA-256 is refused. A release that has no SHA-256 for an extra refuses that
extra, and tells you so. The installer never installs an extra that it did not check.

A refused, a skipped and a failed extra do not stop the driver installation. The driver is installed first. The
extras come last.

## Where the files go

| What | Where |
| --- | --- |
| llama.cpp | `C:\Program Files\amdgpu-wddm\tools\llama` |
| Ollama | `C:\Program Files\amdgpu-wddm\tools\ollama` |
| The model files of llama.cpp | `C:\Users\Public\Documents\amdgpu-wddm\models` |
| The models of Ollama | `%LOCALAPPDATA%\bc250-ollama\models` |

Both extra folders go on the machine search path, so you can start `llama-server` or `ollama` in any command
prompt. The search path changes at the restart that ends the installation.

llama.cpp uses the HIP runtime of this release, which the installer already puts on the machine search path in
`C:\Program Files\amdgpu-wddm\tools\hip`. It carries no copy of its own. Ollama carries its own backends inside
its folder.

Ollama keeps its models in its own format, so it has its own folder for them. The installed Ollama tells you
where. The two extras therefore have two model folders, and the table above gives both.

The models are in a public folder because they are your files. They are large, and you can use them with other
programs. The uninstaller keeps them.

## The questions

The installer asks the questions before it changes anything. It asks them once. The run after a restart has no
window, so it reads the answers from the state of the installation.

```
   llama.cpp for this board - 19 MB download, licence MIT: https://github.com/ggml-org/llama.cpp/...
   Runs GGUF language models on the GPU. llama-server gives you a chat page in your browser; ...
   Install llama.cpp for this board? [Yes/no]:

   Ollama for this board - 50 MB download, licence MIT: https://github.com/ollama/ollama/...
   Keeps a model server running for you and gets models by name. ...
   Install Ollama for this board, set up for this GPU? [Yes/no]:
```

Press Enter for the answer in capitals. The two downloads are small because the models are not in them. After
the extras the installer shows the models:

```
   Models. A model is a large download and it is not needed to install anything else.
   1) Gemma 12B (q4_0, trained for it)         6,5 GB  licence Gemma Terms of Use: ...
      6,5 GB for llama.cpp for this board, plus about as much again for Ollama for this board, which
      keeps its own copy in its own format - about 13,0 GB on the disk in all
      The model this board was measured with.
   2) Qwen3 1.7B (Q4_K_M)                      1,0 GB  licence Apache-2.0: ...
   3) Qwen3.6 35B A3B (IQ2_M)                 10,7 GB  licence Apache-2.0: ...
      needs the 12 GB board memory setting. ...
   Which models shall be downloaded? Numbers or names, separated by commas; Enter for none:
```

The answer for the models is none. A model is a download of gigabytes, so nobody gets one by accident.

**A model chosen while both extras are installed is stored twice.** Ollama keeps models in its own format and reads
only its own folder, so it cannot share the GGUF file that llama.cpp reads. The question says so for each model, with
the sum. Install one extra, or choose the model for one of them and get the other model later, to pay for it once.

## The models

| Id | Model | Size | Licence | Ollama name |
| --- | --- | --- | --- | --- |
| `gemma12b` | Gemma 12B, q4_0, trained for that size | 6,5 GB | Gemma Terms of Use | `gemma4:12b-it-qat` |
| `qwen3-1.7b` | Qwen3 1.7B, Q4_K_M | 1,0 GB | Apache-2.0 | `qwen3:1.7b` |
| `qwen3.6-35b` | Qwen3.6 35B A3B, IQ2_M | 10,7 GB | Apache-2.0 | none |

Gemma 12B is the model this board was measured with. Qwen3 1.7B is small and quick. The 35B model needs the
board memory at 12 GB, which you set in the control application. It does not run with the usual 8 GB.

For llama.cpp the installer gets the GGUF file from Hugging Face and checks its SHA-256 against the list in the
package. For Ollama it gets the name in the last column through our own Ollama. The 35B model has no name there,
because the Ollama library holds no download of that model at this size.

## The switches

An installation without a window gets its answers from the command line.

| Switch | What it does |
| --- | --- |
| `-Components llama,ollama` | Install these extras. Ask nothing. |
| `-NoComponents` | Install no extra. Ask nothing. |
| `-Models gemma12b` | Get these models. Ask nothing. |
| `-NoModels` | Get no model. Ask nothing. |
| `-ComponentDir <folder>` | Take the extras and the models from that folder, and download only what is not there. The models may be in that folder or in a `models` folder inside it, which is where `prepare-offline.cmd` puts them. |

An installation that is run without a window and without any of these switches takes the answer in capitals: both
extras, no model. That is the same answer that pressing Enter gives. `-NoComponents` is how an unattended
installation says no.

Examples:

```
install.cmd -AcceptTestSigning -Components llama -Models gemma12b
install.cmd -AcceptTestSigning -NoComponents
```

An id that this release does not have is reported and skipped. A model that none of the chosen extras can use is
reported and skipped. The setup window gets no extra unless its command line names one.

## An installation without internet

`prepare-offline.cmd` builds a folder that holds everything an installation needs. It takes the same switches:

```
prepare-offline.cmd -Destination D:\amdgpu-wddm-offline -Components llama,ollama -Models gemma12b
```

The folder then holds `components\` and `models\` next to `firmware\`. An installation from that folder finds
them and downloads nothing. Without `-Components` the folder gets the extras that the installer offers by
default. Without `-Models` it gets no model.

`-ComponentDir` reads the same two layouts: the prepared folder itself (its `components\` and `models\`), or one
folder that holds the zips and the model files together, which is the quicker way to put a folder together by hand.
A file that is in the folder but does not match its SHA-256 is refused. A file that is not in the folder at all is
downloaded instead, and the installer says which file that was.

## How to remove them

`uninstall.cmd` removes both extra folders with the install root. It also removes:

- the two entries in the machine search path,
- the logon entry and the settings that an extra wrote for itself.

Neither extra puts anything in the Start menu: both are started from a command prompt, and the search path is what
makes that work.

Each extra removes its own registration first, with its own script, while its folder is still there. The
uninstaller then removes the folders. The list at the end of the uninstall shows every item, and says `kept` or
`LEFT` for each one.

The models stay. They are your files. To remove them as well:

```
uninstall.cmd -RemoveModels
```

That switch clears both model folders of the table above: the GGUF folder and the folder in which Ollama keeps
its own format. It clears the folder of the account that runs the uninstall. You can also remove either model
folder yourself at any time.

## For the release owner

`tools\release\components.json` holds the list. Fill `size` and `sha256` of each extra after its zip is a
release asset of `D-Ogi/amdgpu-wddm`. `build-release.ps1` reads the file, puts the release version into the
asset names and the addresses, and writes the result into the package as `installer\components.json`. It prints
a warning for every extra that has no SHA-256, and `manifest.json` names those extras under
`optional_components.unpinned`. Such a package installs the driver and refuses the extra.

`tools\release\test-components.ps1` is the host test. It runs under `test-dryrun.ps1`. It writes only in its own
work folder and in one scratch registry key, and it uses no network.
