# One-shot kernel dump triage that runs on the lab, not on the development PC. `kd -z` opens the dump
# inside the lab's own Windows, so the development PC's nonpaged pool stays out of it. The README section
# "Why this one runs on the lab" gives the measured hazard. The dump file is only read, never written.
#
#   python tools/win/target.py ps tools/win/kd/analyze-kernel-dump.ps1 -Dump C:\Windows\MEMORY.DMP
#   python tools/win/target.py ps tools/win/kd/analyze-kernel-dump.ps1 -Dump C:\BC250\tmp\kmd.dmp
#       -Pdb C:\BC250\tools\kd\pdb -Out C:\BC250\tmp\kd-triage.txt -Extra "!pool;!vm 1"
#
# Check the script itself here, with no debugger, no dump and no lab:
#   pwsh -NoProfile -File tools/win/kd/analyze-kernel-dump.ps1 -SelfTest
#
# Output: one short text block on stdout, and the full debugger log at -Out on the lab. Keep both as
# evidence. Pull the log with `python tools/win/target.py pull <-Out path> <local path>`.

param(
    [string]$Dump = 'C:\Windows\MEMORY.DMP',
    [string]$Kd = 'C:\BC250\tools\kd\kd.exe',
    [string]$Pdb = 'C:\BC250\tools\kd\pdb',
    [string]$Out = 'C:\BC250\tmp\kd-triage.txt',
    [string]$SymCache = 'C:\BC250\sym',
    [string]$Module = 'bc250kmd',
    [string]$Extra = '',
    [switch]$NoSymbolServer,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$version = 'analyze-kernel-dump 1.1'

# The lines the summary keeps out of the full log. `!analyze -v` prints them with these exact names.
$summaryPattern = 'BUGCHECK_CODE|BUGCHECK_P[0-9]|Bugcheck code|SYMBOL_NAME|MODULE_NAME|FAILURE_BUCKET_ID|' +
                  'IMAGE_NAME|PROCESS_NAME|EXCEPTION_CODE|FAULTING_IP|STACK_TEXT'

function Test-KdModuleName {
    # A module name goes into two debugger commands, so it must not be able to carry a command of its own.
    param([string]$Name)
    return ($Name -match '^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$')
}

function Get-KdCommandList {
    # The commands, in order. `q` must stay last: it is what makes the run one-shot. -More holds the extra
    # commands as one string, because `powershell -File` cannot bind an array (`target.py ps` uses -File).
    # Each `;` in it starts a new line of the command script, which also keeps `.echo` and `.printf` from
    # swallowing the commands behind them.
    param([string]$Name, [string]$More)
    $cmds = @(
        '.symopt+ 0x40'          # SYMOPT_LOAD_ANYTHING: take a PDB whose signature does not match
        ".reload /f $Name.sys"   # load our symbols first, before !analyze guesses
        '!analyze -v'
        '.bugcheck'
        '.lastevent'
        'kv 40'
        "lmvm $Name"
    )
    if ($More) {
        foreach ($c in $More.Split(';')) {
            if ($c.Trim().Length -gt 0) { $cmds += $c.Trim() }
        }
    }
    $cmds += 'q'
    return $cmds
}

function Get-KdSymbolPath {
    # Local cache and our PDB directory always. The public symbol server only when the lab has the network
    # for it: an unreachable server makes every `.reload` wait for a timeout.
    param([string]$Cache, [string]$PdbDir, [bool]$ServerOff)
    $parts = @()
    if ($ServerOff) { $parts += $Cache } else { $parts += "srv*$Cache*https://msdl.microsoft.com/download/symbols" }
    if ($PdbDir) { $parts += $PdbDir }
    return ($parts -join ';')
}

if ($SelfTest) {
    # Offline. It composes the inputs and checks them. It never starts a debugger and needs no dump.
    $fail = 0
    function Assert-KdSelfTest {
        param([string]$What, [bool]$Ok)
        if ($Ok) { Write-Output "PASS $What" } else { Write-Output "FAIL $What"; $script:fail++ }
    }
    Write-Output $version
    Write-Output '--- selftest'

    $c = Get-KdCommandList -Name 'bc250kmd' -More ''
    Assert-KdSelfTest 'no empty -Extra leaves no empty command' (@($c | Where-Object { $_.Trim() -eq '' }).Count -eq 0)
    Assert-KdSelfTest 'q is the last command' ($c[-1] -eq 'q')
    Assert-KdSelfTest 'the module name reaches .reload' ($c -contains '.reload /f bc250kmd.sys')
    Assert-KdSelfTest 'the module name reaches lmvm' ($c -contains 'lmvm bc250kmd')

    $c2 = Get-KdCommandList -Name 'bc250kmd' -More '!pool; ;  !vm 1  ;'
    Assert-KdSelfTest '-Extra keeps its order before q' ($c2[-3] -eq '!pool' -and $c2[-2] -eq '!vm 1')
    Assert-KdSelfTest '-Extra drops empty entries' (@($c2 | Where-Object { $_.Trim() -eq '' }).Count -eq 0)
    Assert-KdSelfTest '-Extra adds one line per command' ($c2.Count -eq $c.Count + 2)

    Assert-KdSelfTest 'a plain module name passes' (Test-KdModuleName 'bc250kmd')
    Assert-KdSelfTest 'a module name with a command is refused' (-not (Test-KdModuleName 'a; .shell cmd'))
    Assert-KdSelfTest 'an empty module name is refused' (-not (Test-KdModuleName ''))

    $sp = Get-KdSymbolPath -Cache 'C:\BC250\sym' -PdbDir 'C:\BC250\tools\kd\pdb' -ServerOff $false
    $want = 'srv*C:\BC250\sym*https://msdl.microsoft.com/download/symbols;C:\BC250\tools\kd\pdb'
    Assert-KdSelfTest 'the symbol path holds cache, server and PDB directory' ($sp -eq $want)
    $spOff = Get-KdSymbolPath -Cache 'C:\BC250\sym' -PdbDir 'C:\BC250\tools\kd\pdb' -ServerOff $true
    Assert-KdSelfTest '-NoSymbolServer leaves no http source' ($spOff -eq 'C:\BC250\sym;C:\BC250\tools\kd\pdb')

    # The summary filter against lines in the shape !analyze -v prints.
    $sample = @(
        'BUGCHECK_CODE:  116',
        'BUGCHECK_P1: ffffffff00000001',
        'MODULE_NAME: bc250kmd',
        'FAILURE_BUCKET_ID:  0x116_IMAGE_bc250kmd.sys',
        'Probably caused by : bc250kmd.sys'
    )
    $hit = @($sample | Select-String -Pattern $summaryPattern)
    Assert-KdSelfTest 'the summary filter keeps the four named lines' ($hit.Count -eq 4)

    if ($fail -eq 0) { Write-Output '--- selftest ok'; exit 0 }
    Write-Output "--- selftest $fail check(s) failed"
    exit 1
}

Write-Output $version
Write-Output ('utc ' + (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
Write-Output ('host ' + $env:COMPUTERNAME)

if (-not (Test-KdModuleName $Module)) {
    Write-Output "refused: -Module '$Module' is not a plain module name"
    exit 2
}
foreach ($pair in @(@('-Dump', $Dump), @('-Kd', $Kd))) {
    if (-not (Test-Path -LiteralPath $pair[1])) {
        Write-Output ("refused: " + $pair[0] + " '" + $pair[1] + "' does not exist")
        exit 2
    }
}
if ($Pdb -and -not (Test-Path -LiteralPath $Pdb)) {
    Write-Output "note: -Pdb '$Pdb' does not exist, going on with the symbol server alone"
    $Pdb = ''
}

$dumpFile = Get-Item -LiteralPath $Dump
Write-Output ('dump ' + $dumpFile.FullName + ' ' + $dumpFile.Length + ' bytes written ' +
              $dumpFile.LastWriteTimeUtc.ToString('o'))

New-Item -ItemType Directory -Force -Path $SymCache | Out-Null
$outDir = Split-Path -Parent $Out
if ($outDir) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

# The commands go in a script file, not in `-c "a;b"`. On a `-c` line `.echo` and `.printf` swallow the
# rest of the line, semicolons and all following commands included, so a harmless -Extra entry can cut
# the run short. See the "How commands are delivered" note in this directory's README.
$cmdFile = "$Out.cmds"
$commands = Get-KdCommandList -Name $Module -More $Extra
Set-Content -LiteralPath $cmdFile -Value $commands -Encoding ASCII

$env:_NT_SYMBOL_PATH = Get-KdSymbolPath -Cache $SymCache -PdbDir $Pdb -ServerOff ([bool]$NoSymbolServer)
$env:DBGHELP_HOMEDIR = $SymCache
$env:_NT_SYMCACHE_PATH = Join-Path $SymCache 'symcache'
Write-Output ('commands ' + ($commands -join ' | '))
Write-Output ('start ' + (Get-Date).ToUniversalTime().ToString('o'))

& $Kd -z $Dump -cf $cmdFile -logo $Out | Out-Null
$code = $LASTEXITCODE

Write-Output ('kd exit ' + $code + ' end ' + (Get-Date).ToUniversalTime().ToString('o'))
if (-not (Test-Path -LiteralPath $Out)) {
    Write-Output "no log at '$Out': kd wrote nothing"
    exit 3
}
Write-Output ('log ' + (Get-Item -LiteralPath $Out).Length + ' bytes at ' + $Out)
Write-Output '--- summary'
Get-Content -LiteralPath $Out | Select-String -Pattern $summaryPattern | ForEach-Object { $_.Line.Trim() }
exit $code
