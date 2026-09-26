& cmd.exe /c C:\BC250\m9\radv-main-bench-new\run.cmd
$nativeExit=$LASTEXITCODE
[IO.File]::WriteAllText('C:\BC250\m9\radv-main-bench-new\worker.exit',[string]$nativeExit)
exit $nativeExit
