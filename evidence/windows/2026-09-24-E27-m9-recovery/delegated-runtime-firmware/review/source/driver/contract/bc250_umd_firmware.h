/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Firmware part of the private caps ABI, shared with the KMD without DRM types.
 * Each unsigned int is a 32-bit wire word; contract and host tests assert layout.
 */
#pragma once

struct bc250_umd_firmware {
    unsigned int me_version,   me_feature;
    unsigned int pfp_version,  pfp_feature;
    unsigned int ce_version,   ce_feature;
    unsigned int mec_version,  mec_feature;
    unsigned int mec2_version, mec2_feature;
    unsigned int rlc_version,  rlc_feature;
    unsigned int sdma_version, sdma_feature;
    unsigned int smc_version;
    unsigned int reserved;
};
