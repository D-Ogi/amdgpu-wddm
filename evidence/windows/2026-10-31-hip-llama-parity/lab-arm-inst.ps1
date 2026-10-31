# One HIP arm on unit A on the INSTALLED files (gate A2 of scratch\t35\release\FINAL-GATES.md): the program runs from
# <InstallRoot>\tools\llama as the installer put it there, with the PATH of a new terminal (machine, then user), so
# Windows takes amdhip64.dll from tools\hip as it does for a user. Same output shape and bound as lab-arm.ps1.
param(
    [ValidateSet('bench', 'chat')][string]$Tool = 'bench',
    [string]$Model = '',
    [string]$ToolArgs = '',
    [string]$EnvSet = '',
    [int]$Bound = 170,
    [string]$Tag = '',
    [string]$InstallRoot = 'C:\Program Files\amdgpu-wddm'
)
$ErrorActionPreference = 'Continue'
$k = 'C:\BC250\tmp\t35hip'
$cli = Join-Path $InstallRoot 'tools\bc250kmd_cli.exe'
$dir = Join-Path $InstallRoot 'tools\llama'
function Now { [DateTime]::UtcNow.ToString('HH:mm:ss') }
function Probe($w) {
    $t = (& $cli clock read 2>&1 | Select-String 'MHz=' | Select-Object -First 1).Line
    "$(Now) $w $t"
}
$run = Join-Path $k 'run-installed'
New-Item -ItemType Directory -Force $run | Out-Null
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
$exeName = @{ bench = 'llama-bench.exe'; chat = 'llama-completion.exe' }[$Tool]
$exe = Join-Path $dir $exeName
if (-not (Test-Path -LiteralPath $exe)) { "no $exe"; exit 2 }
# The loader takes the program folder first, then System32, then PATH. Neither of the first two may hold a runtime.
$found = @()
foreach ($d in @($dir, [Environment]::SystemDirectory) + @($env:Path -split ';' | Where-Object { $_ })) {
    $f = Join-Path $d 'amdhip64.dll'
    if (Test-Path -LiteralPath $f) { $found += $f }
}
"runtime search: $(if ($found.Count) { $found[0] } else { 'none' })$(if ($found.Count -gt 1) { " (then $($found.Count - 1) more)" })"
$a = @()
if ($Model) { $a += '-m'; $a += $Model }
$a += @($ToolArgs -split ' ' | Where-Object { $_ })
foreach ($p in ($EnvSet -split ';' | Where-Object { $_ })) { $n, $v = $p -split '=', 2; Set-Item "env:$n" $v }
"$(Now) arm $Tag llama=installed runtime=installed tool=$Tool env=[$EnvSet] args=[$($a -join ' ')]"
"hashes: ggml-hip $((Get-FileHash (Join-Path $dir 'ggml-hip.dll')).Hash.Substring(0,8)) amdhip64 $(if ($found.Count) { (Get-FileHash $found[0]).Hash.Substring(0,8) } else { 'none' })"
Probe 'before'
$out = Join-Path $run "$Tool.out"; $err = Join-Path $run "$Tool.err"
$t0 = Get-Date
$p = Start-Process -FilePath $exe -ArgumentList $a -WorkingDirectory $run -NoNewWindow -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
$null = $p.Handle   # PS 5.1: without a handle taken before the exit, ExitCode reads back empty
while (-not $p.WaitForExit(5000)) {
    if (((Get-Date) - $t0).TotalSeconds -gt $Bound) { Stop-Process -Id $p.Id -Force; "$(Now) KILLED at bound $Bound s"; break }
}
"$(Now) exit $($p.ExitCode) in $([int]((Get-Date) - $t0).TotalSeconds) s"
Probe 'after'
Get-Content $out | Select-Object -Last 40
Get-Content $err | Where-Object { $_ -match 'error|fail|Error|ggml_cuda_init|found|Device|warning: |tok' } | Select-Object -Last 15
