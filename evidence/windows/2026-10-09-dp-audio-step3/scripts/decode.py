#!/usr/bin/env python3
"""Decode the DP audio register values the lab read, with the field names of the DCN 2.0.1 mask header.

The addresses come from tools/regcalc (the project's only source of register offsets); the field masks and
shifts come from third_party/linux-amdgpu/dcn_2_0_1_sh_mask.h, the same header dpaudio.c and the CLI use.
Usage: python decode.py            (prints the table for the values below)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3] / "bc250-win"
HDR = ROOT / "third_party/linux-amdgpu/dcn_2_0_1_sh_mask.h"

VALUES = [
    ("DP0_DP_SEC_CNTL", "DP0_DP_SEC_CNTL", 0x00001111),
    ("DP0_DP_SEC_AUD_N", "DP0_DP_SEC_AUD_N", 0x00008000),
    ("DP0_DP_SEC_AUD_M_READBACK idle/44.1k", "DP0_DP_SEC_AUD_M_READBACK", 0x00000AB4),
    ("DP0_DP_SEC_AUD_M_READBACK during 48k", "DP0_DP_SEC_AUD_M_READBACK", 0x00000BA8),
    ("DP0_DP_SEC_TIMESTAMP", "DP0_DP_SEC_TIMESTAMP", 0x00000001),
    ("DIG0_AFMT_CNTL", "DIG0_AFMT_CNTL", 0x00000101),
    ("DIG0_AFMT_AUDIO_SRC_CONTROL", "DIG0_AFMT_AUDIO_SRC_CONTROL", 0x00000000),
    ("DIG0_AFMT_AUDIO_PACKET_CONTROL", "DIG0_AFMT_AUDIO_PACKET_CONTROL", 0x00000801),
    ("DIG0_AFMT_AUDIO_PACKET_CONTROL2", "DIG0_AFMT_AUDIO_PACKET_CONTROL2", 0x00000300),
    ("DIG0_AFMT_STATUS", "DIG0_AFMT_STATUS", 0x40000010),
    ("DIG0_AFMT_60958_0", "DIG0_AFMT_60958_0", 0x00000000),
    ("DIG0_AFMT_INFOFRAME_CONTROL0", "DIG0_AFMT_INFOFRAME_CONTROL0", 0x00000000),
    ("DCCG_AUDIO_DTO_SOURCE", "DCCG_AUDIO_DTO_SOURCE", 0x00100010),
    ("DCCG_AUDIO_DTO1_MODULE", "DCCG_AUDIO_DTO1_MODULE", 0x005B8D80),
    ("DCCG_AUDIO_DTO1_PHASE", "DCCG_AUDIO_DTO1_PHASE", 0x0003A980),
    ("AZALIA_AUDIO_DTO", "AZALIA_AUDIO_DTO", 0x00640018),
    ("AZALIA_AUDIO_DTO_CONTROL", "AZALIA_AUDIO_DTO_CONTROL", 0x00000000),
    ("DIO_MEM_PWR_CTRL", "DIO_MEM_PWR_CTRL", 0x00000000),
    ("DC_PINSTRAPS", "DC_PINSTRAPS", 0x00008000),
]


def fields(text, reg):
    """Every FIELD of one register, as (name, shift, mask)."""
    out = {}
    for m in re.finditer(r"^#define\s+(%s)__([A-Za-z0-9_]+)__SHIFT\s+(0x[0-9a-fA-F]+)" % re.escape(reg),
                         text, re.M):
        out.setdefault(m.group(2), {})["shift"] = int(m.group(3), 16)
    for m in re.finditer(r"^#define\s+(%s)__([A-Za-z0-9_]+)_MASK\s+(0x[0-9a-fA-F]+)" % re.escape(reg),
                         text, re.M):
        out.setdefault(m.group(2), {})["mask"] = int(m.group(3), 16)
    return [(n, f["shift"], f["mask"]) for n, f in out.items() if "shift" in f and "mask" in f]


def main():
    text = HDR.read_text(errors="replace")
    for label, reg, value in VALUES:
        fs = sorted(fields(text, reg), key=lambda f: f[1])
        if not fs:
            print("%s = 0x%08X   (no field definition in the header)" % (label, value))
            continue
        print("%s = 0x%08X" % (label, value))
        for name, shift, mask in fs:
            v = (value & mask) >> shift
            width = bin(mask >> shift).count("1")
            if v or width > 1:
                print("    %-44s %d bit%s = %d (0x%X)" % (name, width, "s" if width > 1 else "", v, v))
    return 0


if __name__ == "__main__":
    sys.exit(main())
