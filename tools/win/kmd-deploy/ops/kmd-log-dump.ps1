# Read-only: dumps the KMD log ring (bc250kmd_cli log) to C:\BC250\tmp\kmdlog-<utc>.txt and prints its path,
# line count and the current QPC/UTC pair, so ring timestamps can be placed against receipts.
$ErrorActionPreference = 'Stop'
# The installed release is the only KMD client (owner, 2026-10-08); InstallRoot is what its installer wrote.
$cli = Join-Path ([string](Get-ItemProperty 'HKLM:\SOFTWARE\amdgpu-wddm\Release' -Name InstallRoot -ErrorAction Stop).InstallRoot) 'tools\bc250kmd_cli.exe'
$out = "C:\BC250\tmp\kmdlog-$([DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ')).txt"
$qpc = [Diagnostics.Stopwatch]::GetTimestamp(); $utc = [DateTime]::UtcNow.ToString('o')
& $cli log | Set-Content -LiteralPath $out -Encoding ascii
"exit $LASTEXITCODE out $out lines $((Get-Content -LiteralPath $out).Count) qpc $qpc utc $utc"
