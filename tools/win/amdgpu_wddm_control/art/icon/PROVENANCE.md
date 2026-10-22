PROVENANCE: the application icon of this project, own artwork of the project, published here by its author.

`nagi.ico` is built from `..\nagi\sheet-smile@256.png` of this repository, which `..\nagi\PROVENANCE.md`
records. `make-icon.py` is the recipe: a round badge, the bust on the page colour of our windows
(#101217), clipped to a circle, with a ring in the accent red (#E2263E). The sizes 256, 128, 64, 48 and 40
show head and shoulders. The sizes 32, 24, 20 and 16 take a tighter crop of the face, so the icon stays
readable in the task bar and in a file list.

| File | Bytes | SHA-256 |
|---|---|---|
| `nagi.ico` | 150806 | `6BBDEECDB5C215EDBCFC29FEFBAC771512810D563A9BB07C5A8D792C86CCF188` |
| `make-icon.py` | 2742 | `5057FE1E8FB0877C7833BF1A81CCD1307EB0EC6889072F027E94AA5E69CF212C` |

To build it again: `python -I make-icon.py ..\nagi\sheet-smile@256.png <out dir>`. It needs Pillow. The
icon carries nine images, so the result is not reproducible byte for byte on another Pillow version. The
file in this directory is the one the programs embed.

The icon is the Win32 icon of `amdgpu_wddm_control.exe` and of `amdgpu_wddm_setup.exe` (both builds pass
it to the compiler as `/win32icon`), the icon of the single-file installer, and the icon of the Start menu
and desktop shortcuts, which take it from the application they point at.
