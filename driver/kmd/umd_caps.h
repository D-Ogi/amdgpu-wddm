// Base caps template for DXGKQAITYPE_UMDRIVERPRIVATE. WDDM replaces its historical
// firmware section with metadata cached after this session's PSP load and SMU start.
//
// The bytes live in umd_caps.c. They are bc250_unitA_blob(BC250_MUTATE_NONE) from
// driver/contract/test/bc250_caps_unitA.c, copied once, not retyped. driver/kmd/test/run_umd_caps.ps1
// builds that filler and refuses the run if these bytes are no longer what it produces.
// Do not edit umd_caps.c by hand. Regenerate it with that script's -Generate switch.
#pragma once
#include "../contract/bc250_adapter_identity.h"

#define UMD_CAPS_BYTES 1472u
// offsetof(bc250_umd_private, firmware), checked by umd_caps_test.c.
#define UMD_CAPS_FIRMWARE_OFFSET 1208u
// The CU fields of the device block, which follow the CU mode (cumode.c). offsetof(bc250_umd_private,
// device.cu_active_number, .cu_ao_mask, .cu_bitmap, .cu_ao_bitmap), checked by umd_caps_test.c.
#define UMD_CAPS_CU_ACTIVE_OFFSET 64u
#define UMD_CAPS_CU_AO_MASK_OFFSET 68u
#define UMD_CAPS_CU_BITMAP_OFFSET 72u
#define UMD_CAPS_CU_AO_BITMAP_OFFSET 288u

// offsetof(bc250_umd_private, device.gpu_counter_freq) in kHz, checked by umd_caps_test.c. The UMD reads its
// timestamp frequency from here, so CalibrateGpuClock (wddm.c, BD-056) reports the same number from the same bytes
// instead of a second literal.
#define UMD_CAPS_GPU_COUNTER_FREQ_OFFSET 44u

extern const unsigned char umd_caps_blob[UMD_CAPS_BYTES];

// device.gpu_counter_freq in Hz (the blob is little-endian, as the UMD reads it).
static __inline unsigned long long UmdCapsGpuCounterHz(void)
{
    const unsigned char* p = umd_caps_blob + UMD_CAPS_GPU_COUNTER_FREQ_OFFSET;
    return ((unsigned long long)p[0] | (unsigned long long)p[1] << 8 | (unsigned long long)p[2] << 16 |
            (unsigned long long)p[3] << 24) * 1000ull;
}

#if UMD_CAPS_BYTES != BC250_ADAPTER_IDENTITY_OFFSET
#error Adapter identity trailer must follow the legacy caps exactly
#endif
