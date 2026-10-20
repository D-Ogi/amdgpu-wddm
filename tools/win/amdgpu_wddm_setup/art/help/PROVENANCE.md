PROVENANCE: the pictures of the Smart App Control help card of the setup window (src/HelpCards.cs).
They are screenshots of Windows Security in English Windows 11 24H2 (build 26100), taken for this project,
and show the steps of docs/testing/INSTALL.md, section "Smart App Control and SmartScreen".

This file is written by `../make-help-art.py`, the recipe: each screenshot is scaled to at most
720 px wide, reduced to 256 colours or fewer and saved as PNG without metadata. The same run writes
the WebP pictures of the guide to docs/media.

| File | Size | Bytes | SHA-256 |
|---|---|---|---|
| `sac-3-settings.png` | 720x288 | 28495 | `37AD914ED79ED15961C77747A964DE22623BC87C5FC7558FEAADC7CAED6C5817` |
| `sac-4-confirm.png` | 720x371 | 34152 | `64A0E1F2790DE4EFA72E4EE176F3B3ACED6A0335F12F79FF0DCF94BBBC72D2F0` |
| `sac-5-off.png` | 720x288 | 29369 | `4D70A7ACF70A9696402320EA211E23504E9CF4457671D3174421B18DEA038528` |
| `../make-help-art.py` | - | 5238 | `D79B0C950EC682AF4B2AF75906D68E905E396DC5E07AAB39DD9D57465F0FDCD1` |

To build them again: `python ..\make-help-art.py <screenshots> . <repo>\docs\media`. It needs Pillow,
so the bytes can differ on another Pillow version. The files in this directory are the ones the exe embeds.
