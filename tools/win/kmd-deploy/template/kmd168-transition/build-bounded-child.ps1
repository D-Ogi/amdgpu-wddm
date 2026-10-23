param([string]$Root=$env:BC250_ROOT,[string]$OutDir='',[switch]$TestPipeHolder)
$ErrorActionPreference='Stop'
if($Root){$env:BC250_ROOT=$Root}
if(!$OutDir){throw 'Pass a fresh -OutDir under workspace scratch'}
# Both helpers and the pipe fixture are built with the same recorded toolchain.
& python -B "$PSScriptRoot\..\..\build_helpers.py" --out $OutDir
if($LASTEXITCODE -ne 0){throw "Helper build failed: $LASTEXITCODE"}
