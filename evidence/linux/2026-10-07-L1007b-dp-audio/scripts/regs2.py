#!/usr/bin/env python3
"""L1007b: DCN 2.0.1 audio registers through amdgpu's debugfs register file (reads only). Byte offsets from
`regcalc.py --ip DMU --reg-header dcn_2_0_1_offset.h lookup` (2026-10-07). Adds the Azalia controller block, the DIO
memory power and the DCCG gates to L1007's list: Windows r19 consumes DP audio at 0.33x with the L1007 registers equal."""
import glob, os, struct, sys

REGS = [
    ('DCCG_AUDIO_DTO_SOURCE', 0x005AC), ('DCCG_AUDIO_DTO0_PHASE', 0x005B0), ('DCCG_AUDIO_DTO0_MODULE', 0x005B4),
    ('DCCG_AUDIO_DTO1_PHASE', 0x005B8), ('DCCG_AUDIO_DTO1_MODULE', 0x005BC),
    ('DCCG_GATE_DISABLE_CNTL', 0x004D0), ('DCCG_GATE_DISABLE_CNTL2', 0x004F0),
    ('AZALIA_CONTROLLER_CLOCK_GATING', 0x0E208), ('AZALIA_AUDIO_DTO', 0x0E20C), ('AZALIA_AUDIO_DTO_CONTROL', 0x0E210),
    ('AZALIA_SOCCLK_CONTROL', 0x0E214), ('AZALIA_UNDERFLOW_FILLER_SAMPLE', 0x0E218), ('AZALIA_DATA_DMA_CONTROL', 0x0E21C),
    ('AZALIA_BDL_DMA_CONTROL', 0x0E220), ('AZALIA_RIRB_AND_DP_CONTROL', 0x0E224), ('AZALIA_CORB_DMA_CONTROL', 0x0E228),
    ('AZALIA_APPLICATION_POSITION_IN_CYCLIC_BUFFER', 0x0E244), ('AZALIA_CYCLIC_BUFFER_SYNC', 0x0E248),
    ('AZALIA_GLOBAL_CAPABILITIES', 0x0E24C), ('AZALIA_OUTPUT_PAYLOAD_CAPABILITY', 0x0E250),
    ('AZALIA_OUTPUT_STREAM_ARBITER_CONTROL', 0x0E254),
    ('AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID', 0x0E318), ('AZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID', 0x0E31C),
    ('DIO_MEM_PWR_STATUS', 0x14E74), ('DIO_MEM_PWR_CTRL', 0x14E78), ('DIO_MEM_PWR_CTRL2', 0x14E7C), ('DIO_MEM_PWR_CTRL3', 0x14E84),
    ('DIG0_AFMT_CNTL', 0x15698), ('DIG0_AFMT_AUDIO_SRC_CONTROL', 0x155B4), ('DIG0_AFMT_AUDIO_PACKET_CONTROL', 0x155A8),
    ('DIG0_AFMT_AUDIO_PACKET_CONTROL2', 0x154F0), ('DIG0_AFMT_INFOFRAME_CONTROL0', 0x155B0), ('DIG0_AFMT_60958_0', 0x15580),
    ('DP0_DP_SEC_CNTL', 0x157AC), ('DP0_DP_SEC_AUD_N', 0x157C4), ('DP0_DP_SEC_TIMESTAMP', 0x157D4),
    ('DP0_DP_SEC_AUD_M_READBACK', 0x157D0),
]
path = sorted(glob.glob('/sys/kernel/debug/dri/*/amdgpu_regs'))[0]
fd = os.open(path, os.O_RDONLY)
print(f'# {sys.argv[1] if len(sys.argv) > 1 else ""} {path}')
for name, off in REGS:
    try:
        v = struct.unpack('<I', os.pread(fd, 4, off))[0]
        print(f'{name:52s} 0x{off:05X} 0x{v:08X}')
    except OSError as e:
        print(f'{name:52s} 0x{off:05X} {e}')
