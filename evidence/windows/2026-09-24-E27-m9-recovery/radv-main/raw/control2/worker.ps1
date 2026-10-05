& cmd.exe /c C:\BC250\m9\radv-main-control2\run.cmd
$nativeExit=$LASTEXITCODE
[IO.File]::WriteAllText('C:\BC250\m9\radv-main-control2\worker.exit',[string]$nativeExit)
exit $nativeExit
