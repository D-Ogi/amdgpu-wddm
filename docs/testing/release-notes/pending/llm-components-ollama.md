# Pending release-notes lines: Ollama for this board

This file is not a release. It holds the tester-facing lines for the next tester release notes.
The release step copies the lines into `docs/testing/release-notes/<version>-tester.N.md` and
deletes this file. The installer half of this work has its own pending file,
`llm-components-installer.md`. A release that holds one half without the other keeps only that
half's lines.

## New

- **Ollama comes with the driver.** The installer offers it as an extra, and it is chosen by
  default. Ollama runs language models with one command, holds the models for you and answers
  over a local web interface, which is what most programs that talk to a language model expect.
  This is Ollama itself, an open project under the MIT licence, built for this board: it uses the
  board's graphics processor through the driver instead of the processor cores, and it starts
  when you log in. `bc250-ollama.cmd run gemma4:12b-it-qat` in the installed folder is the whole of
  it. The version it prints ends in `-bc250.<release number>`, so a support report says which build
  you have.

- **Use `bc250-ollama.cmd`, not `ollama.exe`.** It is the same program, with the port and the model
  folder of this installation already set. A bare `ollama.exe` uses the Ollama defaults, so on a
  computer that also has an Ollama of its own it would reach that one instead of ours.

- **The board can be reached two ways, and you can choose.** Ollama can use the driver's graphics
  path or the driver's compute path. Which is faster depends on the model, and both are still
  being improved, so the choice is a setting rather than a fixed decision: the user environment
  variable `BC250_OLLAMA_BACKEND`, with the values `auto`, `vulkan` and `rocm_bc250`. The
  installer writes `auto`, which lets Ollama decide. Stop and start the server after a change.

- **An Ollama you installed yourself is left alone.** Our copy keeps its models in
  `%LOCALAPPDATA%\bc250-ollama\models`, which nothing else is told about, and it takes the next
  free port when something already holds the usual one. The two never share a configuration. They
  do share the board, so run one at a time. Because Ollama keeps models in its own format, a model
  that the installer gets for both Ollama and the llama.cpp extra is stored twice. The installer's
  own question says so, with the sum, before it downloads anything.

- **Removing the driver leaves your models.** The uninstaller deletes the program, the start-at-
  logon entry and the two settings above. Your models are your own data and stay where they are
  until you ask for them to go.

## Known behaviour

- **The first answer after a start is slow.** The model is read from disk into the board's memory
  first. The second question shows the real speed.

- **A model larger than the board's share of memory falls back to the processor cores** and
  becomes very slow rather than failing. The control panel is where the board's share is changed,
  and the largest models want the biggest setting. A 12-billion-parameter model at the usual
  quality setting is about 7 GB and fits in the resting setting.

- **Ollama is a little slower than the driver's own llama.cpp build on the same path**, because it
  adds a server and a chat template around the same computation. The llama.cpp build is in the
  release as its own extra, for measuring.

- **The server writes what happened to** `%LOCALAPPDATA%\bc250-ollama\server.log`. The first
  twenty lines say which part of the board it found and which path it chose. That is the part to
  send with a support report.

- **Stopping the server frees the board's memory.** `bc250-ollama.ps1 stop` ends the part that
  holds the model as well as the server itself, so the board's memory is free for a game or for the
  other extra straight away.
