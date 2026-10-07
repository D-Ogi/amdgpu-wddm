# r19 DP audio at 0.33x (a1: 10 s of audio take 30.6 s at 48 and 44.1 kHz). Read the Azalia controller block, the DIO
# memory power and the DCCG gates, which the KMD's dpaudio observe does not cover. Offsets: dcn_2_0_1_offset.h dword
# offset + cyan_skillfish_ip_offset.h DMU_BASE segment (seg1 0xC0, seg2 0x34C0), times 4. Positive control first:
# DCCG_AUDIO_DTO1_MODULE must read 6000000 (0x005B8D80), the value dpaudio reports. Each read by name first.
$cli = 'C:\Program Files\amdgpu-wddm\tools\bc250kmd_cli.exe'
$regs = [ordered]@{
    'DCCG_AUDIO_DTO1_MODULE' = 0x5BC; 'DCCG_AUDIO_DTO1_PHASE' = 0x5B8; 'DCCG_AUDIO_DTO_SOURCE' = 0x5AC
    'DCCG_GATE_DISABLE_CNTL' = 0x4D0; 'DCCG_GATE_DISABLE_CNTL2' = 0x4F0
    'AZALIA_CONTROLLER_CLOCK_GATING' = 0xE208; 'AZALIA_AUDIO_DTO' = 0xE20C; 'AZALIA_AUDIO_DTO_CONTROL' = 0xE210
    'AZALIA_SOCCLK_CONTROL' = 0xE214; 'AZALIA_UNDERFLOW_FILLER_SAMPLE' = 0xE218; 'AZALIA_DATA_DMA_CONTROL' = 0xE21C
    'AZALIA_BDL_DMA_CONTROL' = 0xE220; 'AZALIA_RIRB_AND_DP_CONTROL' = 0xE224; 'AZALIA_CORB_DMA_CONTROL' = 0xE228
    'AZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER' = 0xE244; 'AZALIA_CYCLIC_BUFFER_SYNC' = 0xE248
    'AZALIA_GLOBAL_CAPABILITIES' = 0xE24C; 'AZALIA_OUTPUT_PAYLOAD_CAPABILITY' = 0xE250; 'AZALIA_OUTPUT_STREAM_ARBITER_CONTROL' = 0xE254
    'DIO_MEM_PWR_STATUS' = 0x14E74; 'DIO_MEM_PWR_CTRL' = 0x14E78; 'DIO_MEM_PWR_CTRL2' = 0x14E7C; 'DIO_MEM_PWR_CTRL3' = 0x14E84
}
foreach ($k in $regs.Keys) {
    $byName = (& $cli read "DCN.mm$k" 2>&1) -join ' '
    $byOff = (& $cli read ('0x{0:X}' -f $regs[$k]) 2>&1) -join ' '
    '{0,-46} 0x{1:X5}  name: {2} | offset: {3}' -f $k, $regs[$k], $byName, $byOff
}
'--- smu clocks'
& $cli dpm 2>&1 | Select-String 'smu metrics'
& $cli smu 2>&1 | Select-Object -First 12
