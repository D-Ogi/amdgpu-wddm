# Read-only: are there Witcher 3 save games on the lab account (counts only), and the current display mode.
$ErrorActionPreference = 'Continue'
$doc = Join-Path $env:USERPROFILE 'Documents\The Witcher 3'
$saves = Join-Path $doc 'gamesaves'
"gamesaves_exists=" + (Test-Path -LiteralPath $saves)
if (Test-Path -LiteralPath $saves) { "save_files=" + ((Get-ChildItem -LiteralPath $saves -File -Filter *.sav -ErrorAction SilentlyContinue | Measure-Object).Count) }
Get-ChildItem -LiteralPath $doc -Directory -ErrorAction SilentlyContinue | ForEach-Object { "dir $($_.Name) files=$((Get-ChildItem -LiteralPath $_.FullName -File -Recurse -ErrorAction SilentlyContinue | Measure-Object).Count)" }
Add-Type -AssemblyName System.Windows.Forms
$b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
"primary_screen=$($b.Width)x$($b.Height)"
"dwm=" + ((Get-Process dwm | Select-Object -First 1).Id)
"witcher3_procs=" + ((Get-Process witcher3 -ErrorAction SilentlyContinue | ForEach-Object { $_.Id }) -join ',')
