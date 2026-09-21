# E18 run 003 on the target: kmtprobe maps an allocation at the GPU VA it chose and holds it; meanwhile the witness
# takes the newest root from the driver's log, walks to that VA and reads the page the leaf names. Read-only witness.
param([string]$Va = '10000000', [string]$Cli = 'C:\BC250\e16-umd\bc250kmd_cli.exe', [string]$Probe = 'C:\BC250\tmp\kmtprobe.exe',
      [string]$Size = '0x10000', [int]$Hold = 60)

$ErrorActionPreference = 'Continue'
$dir = 'C:\BC250\e16\out'
$stamp = (Get-Date).ToString('HHmmss')
$out = Join-Path $dir "kmtprobe-$stamp.txt"
$walk = 'C:\BC250\tmp\e18_walk.ps1'             # pushed next to kmtprobe.exe by the run script

$p = Start-Process $Probe -ArgumentList "--va 0x$Va --size $Size --hold $Hold" -RedirectStandardOutput $out -RedirectStandardError "$out.err" -PassThru -NoNewWindow
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Date) -lt $deadline -and -not (Select-String -Path $out -Pattern '^RESULT|FAILED|failed' -Quiet -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 500 }
"--- kmtprobe so far"
Get-Content $out -ErrorAction SilentlyContinue | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }

$ring = & $Cli log 2>&1
$ring | Out-File (Join-Path $dir "ring-probe-$stamp.log") -Encoding utf8
"--- roots and recent vidmm lines"
$ring | Select-String 'root page table' | Select-Object -Last 3 | ForEach-Object { $_.Line }
$m = $ring | Select-String 'root page table .* = physical 0x([0-9A-Fa-f]+)' | Select-Object -Last 1
if ($m) {
    $root = $m.Matches[0].Groups[1].Value
    "--- walk from root 0x$root to VA 0x$Va"
    $w = & $walk -Root $root -Find $Va -Cli $Cli
    $w | Tee-Object -FilePath (Join-Path $dir "walk-$stamp.txt")
    $leaf = $w | Select-String 'VRAM offset 0x([0-9A-Fa-f]+)' | Select-Object -Last 1
    if ($leaf) {
        "--- the page the leaf names (first nonzero qwords)"
        & $Cli vtable $leaf.Matches[0].Groups[1].Value | Select-Object -First 6 | Tee-Object -FilePath (Join-Path $dir "page-$stamp.txt")
    }
}
if (-not $p.WaitForExit(($Hold + 40) * 1000)) { "kmtprobe still running: killing it"; $p.Kill() }
"--- kmtprobe exit code $($p.ExitCode); tail:"
Get-Content $out -Tail 14 | ForEach-Object { $_.Substring(0, [Math]::Min(200, $_.Length)) }
Get-Content "$out.err" -ErrorAction SilentlyContinue | Select-Object -First 5
