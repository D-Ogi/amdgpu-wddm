@echo off
rem Ollama for this board: the client, pointed at this driver's own server.
rem
rem Use this instead of ollama.exe. The Ollama client reads OLLAMA_HOST, and its default is
rem 127.0.0.1:11434, which is where an Ollama that you installed yourself listens. This driver's
rem server takes the next free port when that one is busy, so a bare ollama.exe would talk to the
rem other installation, or start a server of its own on the processor cores. This file sets the
rem port and the model folder of this installation for this one command, and for nothing else.
rem
rem   bc250-ollama.cmd run gemma4:12b-it-qat "Say hello in one sentence."
rem   bc250-ollama.cmd list
rem
rem The server itself is started, stopped and asked after with bc250-ollama.ps1 next to this file.
setlocal
if not defined BC250_OLLAMA_PORT set BC250_OLLAMA_PORT=11434
set "OLLAMA_HOST=127.0.0.1:%BC250_OLLAMA_PORT%"
set "OLLAMA_MODELS=%LOCALAPPDATA%\bc250-ollama\models"
"%~dp0ollama.exe" %*
set "BC250_OLLAMA_EXIT=%ERRORLEVEL%"
endlocal & exit /b %BC250_OLLAMA_EXIT%
