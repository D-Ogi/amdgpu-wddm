// Base caps template for DXGKQAITYPE_UMDRIVERPRIVATE. WDDM replaces its historical
// firmware section with metadata cached after this session's PSP load and SMU start.
//
// The bytes live in umd_caps.c. They are bc250_unitA_blob(BC250_MUTATE_NONE) from
// driver/contract/test/bc250_caps_unitA.c, copied once, not retyped. driver/kmd/test/run_umd_caps.ps1
// builds that filler and refuses the run if these bytes are no longer what it produces.
// Do not edit umd_caps.c by hand. Regenerate it with that script's -Generate switch.
#pragma once

#define UMD_CAPS_BYTES 1472u
// offsetof(bc250_umd_private, firmware), checked by umd_caps_test.c.
#define UMD_CAPS_FIRMWARE_OFFSET 1208u

extern const unsigned char umd_caps_blob[UMD_CAPS_BYTES];
