# Lab side: one LOG_SUMMARY escape (a Level Two call: dxgkrnl holds the GPU while it runs; use it on an idle lab)
# and the summary lines that match -Pattern.
param([string]$Pattern = 'large pages|vidmm summary: level|refused|snoop mismatches [1-9]')
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
& $cli log summary only 2>&1 | Select-String -Pattern $Pattern | ForEach-Object { $_.Line.Trim() }
