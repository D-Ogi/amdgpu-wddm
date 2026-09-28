// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
HRESULT prepare_blt(const DXGI_DDI_ARG_BLT &,BC250_DXVK_BLT &);
HRESULT prepare_blt1(const DXGI_DDI_ARG_BLT1 &,BC250_DXVK_BLT1 &);
void install_blt_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &);
}
