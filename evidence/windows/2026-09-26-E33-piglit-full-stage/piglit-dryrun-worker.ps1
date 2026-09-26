$ErrorActionPreference='Stop'
$b='C:\BC250\m12\piglit-full001'
$env:PIGLIT_BUILD_DIR="$b\piglit"
$env:PIGLIT_SOURCE_DIR=$env:PIGLIT_BUILD_DIR
$env:PIGLIT_PLATFORM='wgl'
& "$b\python\python.exe" "$b\piglit\piglit" run --dry-run -p wgl -j 1 -1 quick C:\BC250\m12\piglit-full001-dryrun002 > C:\BC250\m12\piglit-full001-dryrun002.log 2> C:\BC250\m12\piglit-full001-dryrun002.err
@{exit_code=$LASTEXITCODE;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json | Set-Content C:\BC250\m12\piglit-full001-dryrun002-result.json
