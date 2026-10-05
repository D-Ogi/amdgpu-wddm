# Decode a DxgKrnl ETW trace for the redirected-blt probe: the Present rows of one process, the Blit_Info
# rows that follow them, the present-history tokens, the GDI standard allocations (the redirection-surface
# witness) and, when the Kernel-Process provider was on, the process names. Runs on the development PC or
# the target with the inbox Microsoft-Windows-DxgKrnl manifest (Get-WinEvent decodes field names itself).
#
#   pwsh decode-redirblt.ps1 -Etl <trace.etl> [-ProcessId <pid>] [-Hwnd <0x...>] [-Out <dir>]
#
# Event ids (ref\presentmon-etw PresentData\ETW\Microsoft_Windows_DxgKrnl.h and the 18990 manifest):
#   Present_Info 184 v1 (hContext, hWindow, VidPnSourceId, FlipInterval, Flags, ReturnStatus, hSrcAllocHandle,
#   hDstAllocHandle); Blit_Info 166 (hwnd, PresentHistoryToken, hSourceAllocation, hDestAllocation, bSubmit,
#   bRedirectedPresent, Flags, rects, SubRectCount); BlitCancel_Info 44; PresentHistory_Start 171,
#   PresentHistory_Info 172, PresentHistoryDetailed_Start 215 (Model: 3 = REDIRECTED_BLT); CddStandardAllocation
#   287 (Width, Height, Format, GdiSurfaceType, GdiSurfaceFlags, Pitch); Kernel-Process ProcessStart 1.
# Mode reading, from PresentMonTraceConsumer.cpp: Blit_Info bRedirectedPresent=1 -> Composed_Copy_CPU_GDI
# (dedicated DX surface, readback); bRedirectedPresent=0 then a token of model 3 or 0 -> Composed_Copy_GPU_GDI;
# bRedirectedPresent=0 and no token -> Hardware_Legacy_Copy_To_Front_Buffer. A Present row with ReturnStatus
# 0xC01E0342 and no Blit_Info is the E45/E46/DWM035 admission refusal.
param(
    [Parameter(Mandatory)][string]$Etl,
    [int]$ProcessId = 0,
    [string]$Hwnd = '',
    [string]$Out = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Etl)) { throw "no $Etl" }
if ($Out -eq '') { $Out = Split-Path -Parent (Resolve-Path -LiteralPath $Etl) }
$stem = [IO.Path]::GetFileNameWithoutExtension($Etl)
$gdiTypes = @{ 0 = 'INVALID'; 1 = 'TEXTURE'; 2 = 'STAGING_CPUVISIBLE'; 3 = 'STAGING'; 4 = 'LOOKUPTABLE'; 5 = 'EXISTINGSYSMEM'; 6 = 'TEXTURE_CPUVISIBLE'; 7 = 'TEXTURE_CROSSADAPTER'; 8 = 'TEXTURE_CPUVISIBLE_CROSSADAPTER' }
$models = @{ 0 = 'UNINITIALIZED'; 1 = 'REDIRECTED_GDI'; 2 = 'REDIRECTED_FLIP'; 3 = 'REDIRECTED_BLT'; 4 = 'REDIRECTED_VISTABLT'; 5 = 'SCREENCAPTUREFENCE'; 6 = 'REDIRECTED_GDI_SYSMEM'; 7 = 'REDIRECTED_COMPOSITION'; 8 = 'SURFACECOMPLETE'; 9 = 'FLIPMANAGER' }

function Fields($e) {
    [xml]$xml = $e.ToXml(); $f = [ordered]@{}
    foreach ($d in $xml.Event.EventData.Data) { $f[[string]$d.Name] = [string]$d.'#text' }
    $f
}
function AsInt($s) { if ($s -match '^0x') { [Convert]::ToInt64($s.Substring(2), 16) } elseif ($s -match '^\d+$') { [int64]$s } else { -1 } }
function Utc($e) { $e.TimeCreated.ToUniversalTime().ToString('HH:mm:ss.fffffff') }

"etl $Etl ($((Get-Item -LiteralPath $Etl).Length) bytes)"
$ids = 184, 166, 44, 171, 172, 215, 287
$xpath = "*[System[Provider[@Name='Microsoft-Windows-DxgKrnl'] and (EventID=" + ($ids -join ' or EventID=') + ")]]"
$events = @(Get-WinEvent -Path $Etl -Oldest -FilterXPath $xpath -ErrorAction SilentlyContinue)
"dxgkrnl rows of interest: $($events.Count)  " + (($events | Group-Object Id | Sort-Object { [int]$_.Name } | ForEach-Object { "$($_.Name)=$($_.Count)" }) -join ' ')

# Process names, when Kernel-Process was collected (ProcessStart 1 carries ImageName); otherwise pids only.
# Pids are reused within a trace (sshd-session.exe comes and goes with every remote command), so a row is
# named after the latest ProcessStart of its pid that precedes the row.
$starts = @{}
foreach ($s in @(Get-WinEvent -Path $Etl -Oldest -FilterXPath "*[System[Provider[@Name='Microsoft-Windows-Kernel-Process'] and EventID=1]]" -ErrorAction SilentlyContinue)) {
    $f = Fields $s
    if (-not ($f.Contains('ProcessID') -and $f.Contains('ImageName'))) { continue }
    $p = [int64](AsInt $f['ProcessID'])
    if (-not $starts.ContainsKey($p)) { $starts[$p] = New-Object System.Collections.ArrayList }
    [void]$starts[$p].Add([pscustomobject]@{ ticks = $s.TimeCreated.ToUniversalTime().Ticks; name = (Split-Path -Leaf $f['ImageName']) })
}
function Who($p, $ticks = [int64]::MaxValue) {
    $p = [int64]$p
    if (-not $starts.ContainsKey($p)) { return "$p" }
    $best = $null
    foreach ($s in $starts[$p]) { if ($s.ticks -le $ticks) { $best = $s } }
    if ($null -eq $best) { return "$p (started before the trace or after this row)" }
    "$p ($($best.name))"
}
if ($ProcessId -and $starts.ContainsKey([int64]$ProcessId)) {
    "ProcessStart rows for pid $ProcessId" + ": " + (($starts[[int64]$ProcessId] | ForEach-Object { ([DateTime]::new($_.ticks, 'Utc')).ToString('HH:mm:ss.fffffff') + ' ' + $_.name }) -join ', ')
}

$rows = foreach ($e in $events) {
    $f = Fields $e
    [pscustomobject]@{ utc = Utc $e; ticks = $e.TimeCreated.ToUniversalTime().Ticks; id = $e.Id; pid = $e.ProcessId; tid = $e.ThreadId; fields = $f }
}
$rows | ForEach-Object { $r = $_; [pscustomobject]@{ utc = $r.utc; id = $r.id; pid = $r.pid; tid = $r.tid; data = (($r.fields.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ') } } |
    Export-Csv -NoTypeInformation -LiteralPath (Join-Path $Out "$stem-redirblt.csv")

# 1. Present rows, by process, and the ones of the probe.
$presents = $rows | Where-Object id -eq 184
"present rows by process: " + (($presents | Group-Object pid | Sort-Object Count -Descending | ForEach-Object { "$(Who $_.Name $_.Group[0].ticks)=$($_.Count)" }) -join ', ')
$mine = $presents
if ($ProcessId) { $mine = $mine | Where-Object pid -eq $ProcessId }
if ($Hwnd) { $h = AsInt $Hwnd; $mine = $mine | Where-Object { (AsInt $_.fields['hWindow']) -eq $h } }
if ($ProcessId -or $Hwnd) {
    "probe present rows: $(@($mine).Count)"
    foreach ($p in $mine) {
        $flags = AsInt $p.fields['Flags']
        $status = AsInt $p.fields['ReturnStatus']
        "  $($p.utc) pid $($p.pid) ctx $($p.fields['hContext']) hwnd $($p.fields['hWindow']) flags 0x{0:X8}{1} status 0x{2:X8} src $($p.fields['hSrcAllocHandle']) dst $($p.fields['hDstAllocHandle'])" -f $flags, $(if ($flags -band 0x10000) { ' [RedirectedBlt]' } else { '' }), ($status -band 0xFFFFFFFF)
    }
}

# 2. Blit_Info and BlitCancel rows near the probe's presents (same pid or hwnd), with the mode reading.
$blits = $rows | Where-Object { $_.id -eq 166 -or $_.id -eq 44 }
$mineBlits = $blits
if ($ProcessId) { $mineBlits = $mineBlits | Where-Object pid -eq $ProcessId }
if ($Hwnd) { $h = AsInt $Hwnd; $mineBlits = $mineBlits | Where-Object { $_.fields.Contains('hwnd') -and (AsInt $_.fields['hwnd']) -eq $h } }
"blit rows: $(@($blits).Count) total, $(@($mineBlits).Count) of the probe"
foreach ($b in $mineBlits) {
    if ($b.id -eq 44) { "  $($b.utc) BlitCancel pid $($b.pid) " + (($b.fields.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' '); continue }
    $redir = $b.fields['bRedirectedPresent']
    $token = AsInt $b.fields['PresentHistoryToken']
    $mode = if ($redir -eq 'true') { 'Composed_Copy_CPU_GDI (readback route)' } elseif ($token -ne 0) { 'token follows: GPU_GDI if model 3/0' } else { 'Hardware_Legacy_Copy_To_Front_Buffer' }
    "  $($b.utc) Blit pid $($b.pid) hwnd $($b.fields['hwnd']) redirected=$redir token=$($b.fields['PresentHistoryToken']) src=$($b.fields['hSourceAllocation']) dst=$($b.fields['hDestAllocation']) submit=$($b.fields['bSubmit']) subrects=$($b.fields['SubRectCount']) -> $mode"
}

# 3. Present-history tokens of model 3 anywhere (the redirected-blt signature), and the probe's models.
$tokens = $rows | Where-Object { $_.id -in 171, 172, 215 }
$byModel = $tokens | Group-Object { $_.fields['Model'] } | Sort-Object { [int]$_.Name } | ForEach-Object { "$($_.Name)/$($models[[int]$_.Name])=$($_.Count)" }
"present-history rows by model: " + ($byModel -join ' ')
$mineTokens = if ($ProcessId) { $tokens | Where-Object pid -eq $ProcessId } else { @() }
foreach ($t in $mineTokens) { "  $($t.utc) id $($t.id) pid $($t.pid) model $($t.fields['Model'])/$($models[[int]$t.fields['Model']]) token $($t.fields['Token']) data $($t.fields['TokenData'])" }
$model3 = @($tokens | Where-Object { $_.fields['Model'] -eq '3' })
"model 3 (REDIRECTED_BLT) tokens in the trace: $($model3.Count)"

# 4. GDI standard allocations: the redirection-surface witness. Match the probe window by its odd size.
$gdi = $rows | Where-Object id -eq 287
"CddStandardAllocation rows: $(@($gdi).Count)"
foreach ($g in $gdi) {
    $t = [int](AsInt $g.fields['GdiSurfaceType'])
    "  $($g.utc) pid $(Who $g.pid $g.ticks) $($g.fields['Width'])x$($g.fields['Height']) format $($g.fields['Format']) type $t/$($gdiTypes[$t]) flags $($g.fields['Flags']) gdiflags $($g.fields['GdiSurfaceFlags']) pitch $($g.fields['Pitch'])"
}
if ($ProcessId) {
    $own = @($gdi | Where-Object pid -eq $ProcessId)
    "CddStandardAllocation rows in the probe's process: $($own.Count)"
}
"csv: " + (Join-Path $Out "$stem-redirblt.csv")
