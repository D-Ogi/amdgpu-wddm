#!/usr/bin/env python3
"""L38 rest: DCN 2.0.1 audio registers read through amdgpu's debugfs register file (reads only). Byte offsets
are regcalc output (scratch/dp-audio/DESIGN.md tables, `regcalc.py --ip DMU`), never typed from memory."""
import glob, os, struct, sys

REGS = [
    ('DCCG_AUDIO_DTO_SOURCE', 0x005AC), ('DCCG_AUDIO_DTO0_PHASE', 0x005B0), ('DCCG_AUDIO_DTO0_MODULE', 0x005B4),
    ('DCCG_AUDIO_DTO1_PHASE', 0x005B8), ('DCCG_AUDIO_DTO1_MODULE', 0x005BC),
    ('AZALIA_F0_CODEC_ROOT_PARAMETER_VENDOR_AND_DEVICE_ID', 0x0E318), ('AZALIA_F0_CODEC_ROOT_PARAMETER_REVISION_ID', 0x0E31C),
    ('DIG0_AFMT_CNTL', 0x15698), ('DIG1_AFMT_CNTL', 0x15A98),
    ('DIG0_AFMT_AUDIO_SRC_CONTROL', 0x155B4), ('DIG1_AFMT_AUDIO_SRC_CONTROL', 0x159B4),
    ('DIG0_AFMT_AUDIO_PACKET_CONTROL', 0x155A8), ('DIG1_AFMT_AUDIO_PACKET_CONTROL', 0x159A8),
    ('DIG0_AFMT_AUDIO_PACKET_CONTROL2', 0x154F0), ('DIG0_AFMT_INFOFRAME_CONTROL0', 0x155B0), ('DIG0_AFMT_60958_0', 0x15580),
    ('DP0_DP_SEC_CNTL', 0x157AC), ('DP1_DP_SEC_CNTL', 0x15BAC),
    ('DP0_DP_SEC_AUD_N', 0x157C4), ('DP1_DP_SEC_AUD_N', 0x15BC4),
    ('DP0_DP_SEC_TIMESTAMP', 0x157D4), ('DP1_DP_SEC_TIMESTAMP', 0x15BD4),
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
