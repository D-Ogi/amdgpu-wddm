// SPDX-License-Identifier: MIT
#include "ddi-blend.h"
#include <initializer_list>
namespace bc250::umd {
#define CHECK_BLEND(name) static_assert(unsigned(D3D10_DDI_BLEND_##name)==unsigned(D3D11_BLEND_##name))
CHECK_BLEND(ZERO); CHECK_BLEND(ONE); CHECK_BLEND(SRC_COLOR); CHECK_BLEND(INV_SRC_COLOR);
CHECK_BLEND(SRC_ALPHA); CHECK_BLEND(INV_SRC_ALPHA); CHECK_BLEND(DEST_ALPHA); CHECK_BLEND(INV_DEST_ALPHA);
CHECK_BLEND(DEST_COLOR); CHECK_BLEND(INV_DEST_COLOR); CHECK_BLEND(BLEND_FACTOR);
CHECK_BLEND(SRC1_COLOR); CHECK_BLEND(INV_SRC1_COLOR); CHECK_BLEND(SRC1_ALPHA); CHECK_BLEND(INV_SRC1_ALPHA);
CHECK_BLEND(OP_ADD); CHECK_BLEND(OP_SUBTRACT); CHECK_BLEND(OP_REV_SUBTRACT); CHECK_BLEND(OP_MIN); CHECK_BLEND(OP_MAX);
#undef CHECK_BLEND
static_assert(unsigned(D3D10_DDI_BLEND_SRC_ALPHASAT)==unsigned(D3D11_BLEND_SRC_ALPHA_SAT));
static_assert(unsigned(D3D10_DDI_BLEND_INVBLEND_FACTOR)==unsigned(D3D11_BLEND_INV_BLEND_FACTOR));
#define CHECK_LOGIC(name) static_assert(unsigned(D3D11_1_DDI_LOGIC_OP_##name)==unsigned(D3D11_LOGIC_OP_##name))
CHECK_LOGIC(CLEAR); CHECK_LOGIC(SET); CHECK_LOGIC(COPY); CHECK_LOGIC(COPY_INVERTED);
CHECK_LOGIC(NOOP); CHECK_LOGIC(INVERT); CHECK_LOGIC(AND); CHECK_LOGIC(NAND);
CHECK_LOGIC(OR); CHECK_LOGIC(NOR); CHECK_LOGIC(XOR); CHECK_LOGIC(EQUIV);
CHECK_LOGIC(AND_REVERSE); CHECK_LOGIC(AND_INVERTED); CHECK_LOGIC(OR_REVERSE); CHECK_LOGIC(OR_INVERTED);
#undef CHECK_LOGIC
static_assert(D3D10_DDI_SIMULTANEOUS_RENDER_TARGET_COUNT==D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
static_assert(unsigned(D3D10_DDI_COLOR_WRITE_ENABLE_ALL)==unsigned(D3D11_COLOR_WRITE_ENABLE_ALL));
namespace {
HRESULT factor(D3D10_DDI_BLEND f) {
    // These later DDI constants have no D3D11 COM counterpart. Do not silently
    // turn alpha-factor semantics into a four-component blend constant.
    if (f==D3D10_DDI_BLEND_ALPHA_FACTOR || f==D3D10_DDI_BLEND_INVALPHA_FACTOR) return E_NOTIMPL;
    return ((f>=D3D10_DDI_BLEND_ZERO && f<=D3D10_DDI_BLEND_SRC_ALPHASAT) ||
        (f>=D3D10_DDI_BLEND_BLEND_FACTOR && f<=D3D10_DDI_BLEND_INV_SRC1_ALPHA)) ? S_OK : E_INVALIDARG;
}
}
HRESULT convert_blend(const D3D11_1_DDI_BLEND_DESC &s,D3D11_BLEND_DESC1 &out) {
    D3D11_BLEND_DESC1 d{};
    d.AlphaToCoverageEnable=s.AlphaToCoverageEnable;
    d.IndependentBlendEnable=s.IndependentBlendEnable;
    for (UINT i=0;i<D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;++i) {
        const auto &a=s.RenderTarget[s.IndependentBlendEnable ? i : 0];
        auto &b=d.RenderTarget[i];
        if (a.BlendEnable && a.LogicOpEnable) return E_INVALIDARG;
        if (a.RenderTargetWriteMask & ~D3D11_COLOR_WRITE_ENABLE_ALL) return E_INVALIDARG;
        b.BlendEnable=a.BlendEnable; b.LogicOpEnable=a.LogicOpEnable;
        b.RenderTargetWriteMask=a.RenderTargetWriteMask;
        b.SrcBlend=b.SrcBlendAlpha=D3D11_BLEND_ONE;
        b.DestBlend=b.DestBlendAlpha=D3D11_BLEND_ZERO;
        b.BlendOp=b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
        b.LogicOp=D3D11_LOGIC_OP_NOOP;
        if (a.BlendEnable) {
            for (auto f : {a.SrcBlend,a.DestBlend,a.SrcBlendAlpha,a.DestBlendAlpha}) {
                const HRESULT hr=factor(f); if (FAILED(hr)) return hr;
            }
            if (a.BlendOp<D3D10_DDI_BLEND_OP_ADD || a.BlendOp>D3D10_DDI_BLEND_OP_MAX ||
                a.BlendOpAlpha<D3D10_DDI_BLEND_OP_ADD || a.BlendOpAlpha>D3D10_DDI_BLEND_OP_MAX) return E_INVALIDARG;
            b.SrcBlend=static_cast<D3D11_BLEND>(a.SrcBlend); b.DestBlend=static_cast<D3D11_BLEND>(a.DestBlend);
            b.SrcBlendAlpha=static_cast<D3D11_BLEND>(a.SrcBlendAlpha); b.DestBlendAlpha=static_cast<D3D11_BLEND>(a.DestBlendAlpha);
            b.BlendOp=static_cast<D3D11_BLEND_OP>(a.BlendOp); b.BlendOpAlpha=static_cast<D3D11_BLEND_OP>(a.BlendOpAlpha);
        }
        if (a.LogicOpEnable) {
            if (a.LogicOp<D3D11_1_DDI_LOGIC_OP_CLEAR || a.LogicOp>D3D11_1_DDI_LOGIC_OP_OR_INVERTED) return E_INVALIDARG;
            b.LogicOp=static_cast<D3D11_LOGIC_OP>(a.LogicOp);
        }
    }
    out=d; return S_OK;
}
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11_1_DDI_BLEND_DESC *) { return sizeof(DdiBlend); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11_1_DDI_BLEND_DESC *desc,
    D3D10DDI_HBLENDSTATE handle,D3D10DDI_HRTBLENDSTATE) {
    auto *s=static_cast<DdiBlend *>(handle.pDrvPrivate);
    if (s) s->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !desc || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        D3D11_BLEND_DESC1 d{};
        HRESULT hr=convert_blend(*desc,d);
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        hr=owner.device()->CreateBlendState1(&d,&s->object);
        if (FAILED(hr)) {
            if (s->object) { s->object->Release(); s->object=nullptr; }
            report_ddi_error(owner,hr);
        } else if (!s->object) report_ddi_error(owner,E_FAIL);
    });
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HBLENDSTATE handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiBlend *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
void APIENTRY bind(D3D10DDI_HDEVICE h,D3D10DDI_HBLENDSTATE handle,const FLOAT factors[4],UINT mask) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *s=static_cast<DdiBlend *>(handle.pDrvPrivate);
        context.OMSetBlendState(s ? s->object : nullptr,factors,mask);
    });
}
}
void install_blend_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateBlendStateSize=size; t.pfnCreateBlendState=create;
    t.pfnDestroyBlendState=destroy; t.pfnSetBlendState=bind;
}
}
