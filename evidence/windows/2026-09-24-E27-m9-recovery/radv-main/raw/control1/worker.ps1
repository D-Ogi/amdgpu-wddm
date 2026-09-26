& cmd.exe /c C:\BC250\m9\radv-main-control1\run.cmd
$nativeExit=$LASTEXITCODE
[IO.File]::WriteAllText('C:\BC250\m9\radv-main-control1\worker.exit',[string]$nativeExit)
exit $nativeExit
