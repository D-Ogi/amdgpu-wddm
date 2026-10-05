param([Parameter(Mandatory)][ValidateSet('Cpu','Gpu')][string]$Phase)
$ErrorActionPreference='Stop';$d=$PSScriptRoot
try {
 if($d -ine 'C:\BC250\m14\window002' -or (Get-Process -Id $PID).SessionId -eq 0){throw 'Wrong interactive session/directory'}
 Remove-Item Env:BC250_M14_RUNTIME_PROBE -ErrorAction SilentlyContinue
 Remove-Item Env:BC250_M14_CAPTURE_DUMP -ErrorAction SilentlyContinue
 if($Phase -eq 'Gpu'){$env:BC250_M14_RUNTIME_PROBE='1';$env:BC250_M14_CAPTURE_DUMP='1'}
 $env:DXVK_SHADER_CACHE='0';$env:MESA_SHADER_CACHE_DISABLE='true';$env:DXVK_LOG_PATH=$d;$env:DXVK_LOG_LEVEL='info'
 $args=@('25',"$d\d3d11bench.exe",'--mode','window','--adapter','1002:13fe','--size','64x64','--scenes','draws,fill,shaders','--draws','8','--layers','2','--shaders','4','--frames','3','--warmup','0','--deadline','20','--out',"$d\$Phase.json",'--dump',"$d\$Phase-images")
 & "$d\debug-child.exe" @args > "$d\$Phase-debug.txt" 2> "$d\$Phase-stderr.txt"
 if($LASTEXITCODE -ne 0){throw "Debugged client exit $LASTEXITCODE"}
} catch {
 $_|Out-String|Set-Content "$d\$Phase-console.err"
 exit 1
}
