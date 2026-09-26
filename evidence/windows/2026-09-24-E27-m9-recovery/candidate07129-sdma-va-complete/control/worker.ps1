& cmd.exe /c C:\BC250\m9\candidate07129-control\run.cmd
$nativeExit=$LASTEXITCODE
[IO.File]::WriteAllText('C:\BC250\m9\candidate07129-control\worker.exit',[string]$nativeExit)
exit $nativeExit
