& cmd.exe /c C:\BC250\m9\radv-main-bench-old\run.cmd
$nativeExit=$LASTEXITCODE
[IO.File]::WriteAllText('C:\BC250\m9\radv-main-bench-old\worker.exit',[string]$nativeExit)
exit $nativeExit
