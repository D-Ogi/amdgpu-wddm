# amdgpu-wddm Setup

The setup window of a tester release: install, update and repair the amdgpu-wddm driver on the ASRock BC-250, and
prepare a folder for a PC without internet. It is a face for the release's PowerShell engine
(`tools/release/installer/install.ps1`, `prepare-offline.ps1`): every decision, check and change is the engine's, the
window shows them in plain words. Contract: `docs/gui/interfaces-setup.md`.

| Screen | What it shows |
|---|---|
| Welcome | What the release is, where its files come from (this package or a prepared folder, checked before use), the entry to prepare an offline folder |
| Checking | The engine's plan run (`-Plan`): read only, nothing changes |
| Review | Checks that need attention, what the run will do (restarts, firmware source), the settings each run keeps, changes or sets (from the engine's settings-impact plan), the release notes, the consents (test signing, BitLocker) |
| Install | Stages as the engine reports them, cancel while the engine allows it |
| Restart | A planned restart (never forced): restart now or later; after sign-in the release's RunOnce entry starts the window with `--continue` |
| Done | One of five outcomes (A3): success, information, problem, cancelled, restart; whether anything changed; the next step (retry, repair from amdgpu-wddm Control, support file) |
| Prepare | `--prepare-offline`: a destination, an optional firmware folder, the copy, the result |

Ordinary words only: no driver internals, codes or file names in the window (G-NOINT, checked in every language and
every screen by the build). The details are in the support file, a zip that the user saves on request: the window's
summary and, per engine run, the command line, events, result, engine output and log, with the user name, computer
name and profile path redacted.

## How it runs the engine

Each run gets a fresh invocation id and its own folder (`--run-root`, default `%TEMP%\amdgpu-wddm-setup`). The window
starts Windows PowerShell 5.1 from System32 hidden, with `-Gui -InvocationId <id> -EventsFile -ResultFile`, reads the
complete event lines as they come and, at the end, the terminal result. A result counts only if its invocation id is
this run's (A2): a stale or missing result reads "the installer stopped before it finished", and the window then says
that changes may have been made; "nothing changed" is said only on a bound result that reports no change (A3).
Cancel writes `<events>.cancel`, and only while the engine says it can stop. The window cannot be closed while the engine runs.

The window starts as the invoking user (`asInvoker`) and relaunches itself elevated at once (one UAC prompt);
`--prepare-offline` stays unelevated.

## Build and test

```
pwsh tools\win\amdgpu_wddm_setup\build.ps1 -Out P:\BC-250\scratch\build\gui-setup\setup-app
pwsh tools\release\build-release.ps1 ... -SetupApp P:\BC-250\scratch\build\gui-setup\setup-app
```

The build is the gate (its header lists the four steps): unit tests, the compile, the engine client against a
scripted fake engine (`test/fake-engine.ps1`) and every screen drawn headless at 96, 120, 144 and 192 DPI in four
languages and at a 150 % text size. No step shows a window. `tools/release/test-dryrun.ps1` also runs the built window
against the real engine (plan and dry run) when the package carries it.

Translations: `strings/strings.<lang>.txt`. A new or redone translation gets `?` as its hash and `strings/stamp.ps1`
stamps it; a line whose English text changed fails G-STR until it is retranslated. Polish, Japanese and Korean are
machine-translated (`mt`) until a reviewer marks them `rev`.

Mierz siły na zamiary: the setup window promises only what the engine reported.
(Measure your strength against your intentions.)
