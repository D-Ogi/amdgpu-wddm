# Evidence

Raw output from hardware. Rules: `docs/01-evidence-rules.md`.

- `linux/YYYY-MM-DD-Exx-name/` and `windows/YYYY-MM-DD-Exx-name/`, one directory per run.
- For diagnostic USB runs copy the whole `bc250/results/run-NNN/` directory from the stick: `report.json`, `summary.json`, `dmesg.txt`, `amdgpu_firmware_info.txt`, `qr-chunks.txt`, `launch.log`.
- Before committing, remove what is private and irrelevant to the GPU (MAC addresses, Wi-Fi SSIDs, serial numbers) and note the removal in a `REDACTED.txt` in the same directory.
- Files are never edited after the commit that adds them.
- Exception recorded once: on 2026-09-26, before the first public push, the git history was rewritten (commit metadata only, trees unchanged) and every commit hash cited in the write-ups here was mechanically replaced by its new value; nothing else in these files was touched.
