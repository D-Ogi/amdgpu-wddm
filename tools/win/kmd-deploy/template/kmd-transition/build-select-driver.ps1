param([string]$Root=$env:BC250_ROOT,[string]$OutDir='')
$ErrorActionPreference='Stop'
if($Root){$env:BC250_ROOT=$Root}
if(!$OutDir){throw 'Pass a fresh -OutDir under workspace scratch'}
& python -B "$PSScriptRoot\..\..\build_helpers.py" --out $OutDir
if($LASTEXITCODE -ne 0){throw "Helper build failed: $LASTEXITCODE"}
