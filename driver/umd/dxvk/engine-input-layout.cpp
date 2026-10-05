// SPDX-License-Identifier: MIT
#include "engine-input-layout.h"
#include <cstddef>
#include <cstring>
#include <type_traits>
using namespace bc250::umd;
#define MATCH(A, a, B, b) static_assert(offsetof(A,a)==offsetof(B,b))
static_assert(sizeof(BC250_DXVK_SIGNATURE_ENTRY)==sizeof(D3D11_1DDIARG_SIGNATURE_ENTRY2));
MATCH(BC250_DXVK_SIGNATURE_ENTRY,SystemValue,D3D11_1DDIARG_SIGNATURE_ENTRY2,SystemValue);
MATCH(BC250_DXVK_SIGNATURE_ENTRY,Register,D3D11_1DDIARG_SIGNATURE_ENTRY2,Register);
MATCH(BC250_DXVK_SIGNATURE_ENTRY,Mask,D3D11_1DDIARG_SIGNATURE_ENTRY2,Mask);
MATCH(BC250_DXVK_SIGNATURE_ENTRY,Stream,D3D11_1DDIARG_SIGNATURE_ENTRY2,Stream);
MATCH(BC250_DXVK_SIGNATURE_ENTRY,ComponentType,D3D11_1DDIARG_SIGNATURE_ENTRY2,RegisterComponentType);
MATCH(BC250_DXVK_SIGNATURE_ENTRY,MinPrecision,D3D11_1DDIARG_SIGNATURE_ENTRY2,MinPrecision);
static_assert(sizeof(BC250_DXVK_SO_ENTRY)==sizeof(D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY));
MATCH(BC250_DXVK_SO_ENTRY,Stream,D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY,Stream);
MATCH(BC250_DXVK_SO_ENTRY,OutputSlot,D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY,OutputSlot);
MATCH(BC250_DXVK_SO_ENTRY,RegisterIndex,D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY,RegisterIndex);
MATCH(BC250_DXVK_SO_ENTRY,RegisterMask,D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY,RegisterMask);
#undef MATCH
namespace {
struct Lookup { IBc250DxvkDevice *engine; HRESULT result=S_OK; };
VertexFormat lookup(void *user, DXGI_FORMAT format) {
    auto &state=*static_cast<Lookup *>(user);
    VertexFormat result;
    state.result=state.engine->GetVertexFormat(format,&result.format,&result.bytes);
    return FAILED(state.result) ? VertexFormat{} : result;
}
}
HRESULT bc250_create_engine_input_layout(IBc250DxvkDevice *engine,
    const D3D10DDIARG_CREATEELEMENTLAYOUT &input, ID3D11InputLayout **output) {
    if (!output) return E_INVALIDARG;
    *output=nullptr;
    if (!engine) return E_INVALIDARG;
    Lookup state{engine};
    InputLayout layout;
    HRESULT hr=translate_input_layout(input,lookup,&state,layout);
    if (FAILED(state.result)) return state.result;
    if (FAILED(hr)) return hr;
    std::array<BC250_DXVK_VERTEX_ATTRIBUTE,vertex_attribute_limit> attributes{};
    std::array<BC250_DXVK_VERTEX_BINDING,vertex_binding_limit> bindings{};
    for (unsigned i=0;i<layout.attribute_count;++i) {
        const auto &a=layout.attributes[i];
        attributes[i]={a.location,a.binding,a.format,a.offset};
    }
    for (unsigned i=0;i<layout.binding_count;++i) {
        const auto &b=layout.bindings[i];
        bindings[i]={b.binding,b.extent,b.inputRate,b.divisor};
    }
    BC250_DXVK_INPUT_LAYOUT desc{layout.attribute_count,attributes.data(),layout.binding_count,bindings.data()};
    return engine->CreateInputLayout(&desc,output);
}
