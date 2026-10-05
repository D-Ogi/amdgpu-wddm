// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include <cstring>
namespace bc250::umd {
struct DdiQuery { ID3D11Query *object=nullptr; ID3D11Predicate *predicate=nullptr; bool legacy_pipeline=false; };
HRESULT convert_query(const D3D10DDIARG_CREATEQUERY &,D3D11_QUERY_DESC &,bool &predicate);
HRESULT query_ddi_status(HRESULT);
// Engine EVENT queries may write FALSE while returning S_FALSE. The DDI
// contract requires leaving the caller buffer unchanged until S_OK.
template<typename Read> HRESULT read_query_result(void *out,UINT bytes,Read &&read) {
    alignas(D3D11_QUERY_DATA_PIPELINE_STATISTICS) unsigned char temporary[sizeof(D3D11_QUERY_DATA_PIPELINE_STATISTICS)]{};
    if ((!out && bytes) || bytes>sizeof(temporary)) return E_INVALIDARG;
    HRESULT hr=read(out ? temporary : nullptr,bytes);
    if (hr==S_OK && out) std::memcpy(out,temporary,bytes);
    return hr;
}
void install_query_ddi(D3D11_1DDI_DEVICEFUNCS &);
}
