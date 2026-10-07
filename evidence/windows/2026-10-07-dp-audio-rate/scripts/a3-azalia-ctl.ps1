# r19 DP audio at 0.33x and silent. KMD 0.7.216.2 (kmd/audio-controller-reads) allows reads of the Azalia controller
# block, the DIO memory power and the DCCG gates. Read them by name (offsets from the generated table only) idle and
# 4 s into a 10 s 48 kHz tone, next to the Linux L1007b values. Positive control first: DCCG_AUDIO_DTO1_MODULE.
$ErrorActionPreference = 'Continue'
$cli = 'C:\BC250\dpaudio\bc250kmd_cli.exe'
$dir = 'C:\BC250\tmp\audio'
$names = 'DCCG_AUDIO_DTO1_MODULE', 'DCCG_AUDIO_DTO1_PHASE', 'DCCG_AUDIO_DTO_SOURCE', 'DCCG_GATE_DISABLE_CNTL', 'DCCG_GATE_DISABLE_CNTL2',
    'AZALIA_CONTROLLER_CLOCK_GATING', 'AZALIA_AUDIO_DTO', 'AZALIA_AUDIO_DTO_CONTROL', 'AZALIA_SOCCLK_CONTROL',
    'AZALIA_UNDERFLOW_FILLER_SAMPLE', 'AZALIA_DATA_DMA_CONTROL', 'AZALIA_BDL_DMA_CONTROL', 'AZALIA_RIRB_AND_DP_CONTROL',
    'AZALIA_CORB_DMA_CONTROL', 'AZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER', 'AZALIA_CYCLIC_BUFFER_SYNC',
    'AZALIA_GLOBAL_CAPABILITIES', 'AZALIA_OUTPUT_PAYLOAD_CAPABILITY', 'AZALIA_OUTPUT_STREAM_ARBITER_CONTROL',
    'DIO_MEM_PWR_STATUS', 'DIO_MEM_PWR_CTRL', 'DIO_MEM_PWR_CTRL2', 'DIO_MEM_PWR_CTRL3', 'DP0_SEC_CNTL'
function ReadAll([string]$tag) { foreach ($n in $names) { '{0} {1}' -f $tag, ((& $cli read "DMU.mm$n" 2>&1) -join ' ') } }
& $cli version 2>&1 | Select-Object -First 2
ReadAll 'idle'
if (-not (Test-Path "$dir\t48.wav")) { 'no t48.wav (run a1 first)'; exit 2 }
$p = New-Object Media.SoundPlayer "$dir\t48.wav"; $p.Load()
$job = Start-Job -ArgumentList $cli, (, $names) -ScriptBlock { param($cli, $names) Start-Sleep -Seconds 4; foreach ($n in $names) { 'play ' + ((& $cli read "DMU.mm$n" 2>&1) -join ' ') }; & $cli dpaudio 2>&1 }
$sw = [Diagnostics.Stopwatch]::StartNew()
try { $p.PlaySync(); $err = '' } catch { $err = $_.Exception.Message }
't48: PlaySync {0:N3} s for 10.000 s of audio {1}' -f $sw.Elapsed.TotalSeconds, $err
Receive-Job $job -Wait; Remove-Job $job
