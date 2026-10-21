param([string]$Root=$(if ($env:BC250_ROOT) { $env:BC250_ROOT } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }),[string]$Out="$Root\scratch\build\dcn-write-sites",[string]$Source='')
# BD-090: every register a call of MmioDcnWrite or MmioDcnWriteEx names is on the list that function admits
# (check_dcn_write_sites.py). -Source is another directory of KMD .c files, for the base peel; the register lists
# stay this tree's.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
New-Item -ItemType Directory -Force $Out | Out-Null
$arguments=@('-B',"$PSScriptRoot\check_dcn_write_sites.py",'--regs',"$repo\driver\kmd\regs.generated.h")
if($Source){$arguments+=@('--source',$Source)}
& python @arguments 2>&1 | Tee-Object -FilePath (Join-Path $Out 'result.txt')
exit $LASTEXITCODE
