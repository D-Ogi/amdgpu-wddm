# Ollama for your BC-250 board

This is Ollama, built for this board and for this display driver. It runs language models on the
board's graphics processor instead of on the processor cores, which is between five and fifteen
times faster for most models.

Ollama itself is not ours. It is an open project, under the MIT licence, and `LICENSE` next to
this file is its licence. What we changed and why is in the driver's own documentation.

## Starting it

The driver's installer already set this up: the server starts when you log in, and you can use it
straight away. Open a command prompt in this folder and ask it something:

```
bc250-ollama.cmd run gemma4:12b-it-qat
```

Use `bc250-ollama.cmd`, not `ollama.exe`. It is the same program, with the port and the model
folder of this installation already set. `ollama.exe` on its own uses the Ollama defaults, and on a
machine that also has an Ollama of its own it would reach that one instead of this server.

If the server is not running, start it by hand:

```
powershell -ExecutionPolicy Bypass -File bc250-ollama.ps1 start
```

The other commands are `stop`, `status`, and `pull -Model <name>` to fetch a model.
`status` answers in one line of JSON, which is there for the driver's control panel rather than
for reading.

## Where the models go

```
%LOCALAPPDATA%\bc250-ollama\models
```

A 12-billion-parameter model is about 8 GB, so keep an eye on the space on that drive. Removing
the driver does not delete your models. You delete them yourself, or you ask the uninstaller to
with its own switch.

## Which part of the board does the work

The board can be reached two ways, and both are in this folder:

- **Vulkan**, through the driver's graphics path.
- **HIP**, through the driver's compute path.

Which one is faster depends on the model, and both are still being improved, so the choice is a
setting rather than a fixed decision:

```
BC250_OLLAMA_BACKEND = auto        let Ollama decide
                     = vulkan      always the graphics path
                     = rocm_bc250  always the compute path
```

It is an ordinary user environment variable, so you can change it in the Windows settings for
environment variables, or in the driver's control panel. Stop and start the server afterwards.
`auto` is what the installer writes.

## Three things worth knowing

- **Memory.** The board shares its memory with Windows. A model that needs more than the board's
  share falls back to the processor cores and becomes very slow. The driver's control panel is
  where you change how much memory the board receives, and the largest models want the biggest
  setting.
- **The first answer is slow.** The model has to be read from disk into the board's memory first.
  The second question is the one with the real speed.
- **An Ollama you installed yourself.** This copy does not touch it. It has its own models folder
  and, if yours is already running, its own port. The two do not interfere, but they do compete
  for the board, so run one at a time.

## If something does not work

The server writes what happened to:

```
%LOCALAPPDATA%\bc250-ollama\server.log
```

The first twenty lines say which part of the board it found and which backend it chose. That is
the part to send with a support report.
