// The caps blob DxgkDdiQueryAdapterInfo returns for DXGKQAITYPE_UMDRIVERPRIVATE.
//
// The bytes live in umd_caps.c. They are bc250_unitA_blob(BC250_MUTATE_NONE) from
// driver/contract/test/bc250_caps_unitA.c, copied once, not retyped. driver/kmd/test/run_umd_caps.ps1
// builds that filler and refuses the run if these bytes are no longer what it produces.
// Do not edit umd_caps.c by hand. Regenerate it with that script's -Generate switch.
#pragma once

#define UMD_CAPS_BYTES 1472u

extern const unsigned char umd_caps_blob[UMD_CAPS_BYTES];
