// SPDX-License-Identifier: MIT
#include "ddi-sampler.h"
#include <array>
namespace bc250::umd {
static_assert(unsigned(D3D10_DDI_COMPARISON_ALWAYS)==unsigned(D3D11_COMPARISON_ALWAYS));
static_assert(unsigned(D3D10_DDI_COMPARISON_EQUAL)==unsigned(D3D11_COMPARISON_EQUAL));
static_assert(unsigned(D3D10_DDI_COMPARISON_GREATER)==unsigned(D3D11_COMPARISON_GREATER));
static_assert(unsigned(D3D10_DDI_COMPARISON_GREATER_EQUAL)==unsigned(D3D11_COMPARISON_GREATER_EQUAL));
static_assert(unsigned(D3D10_DDI_COMPARISON_LESS)==unsigned(D3D11_COMPARISON_LESS));
static_assert(unsigned(D3D10_DDI_COMPARISON_LESS_EQUAL)==unsigned(D3D11_COMPARISON_LESS_EQUAL));
static_assert(unsigned(D3D10_DDI_COMPARISON_NEVER)==unsigned(D3D11_COMPARISON_NEVER));
static_assert(unsigned(D3D10_DDI_COMPARISON_NOT_EQUAL)==unsigned(D3D11_COMPARISON_NOT_EQUAL));
static_assert(unsigned(D3D10_DDI_FILTER_ANISOTROPIC)==unsigned(D3D11_FILTER_ANISOTROPIC));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_ANISOTROPIC)==unsigned(D3D11_FILTER_COMPARISON_ANISOTROPIC));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT)==unsigned(D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR)==unsigned(D3D11_FILTER_COMPARISON_MIN_LINEAR_MAG_POINT_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT)==unsigned(D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR)==unsigned(D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_MAG_MIP_POINT)==unsigned(D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR)==unsigned(D3D11_FILTER_COMPARISON_MIN_MAG_POINT_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT)==unsigned(D3D11_FILTER_COMPARISON_MIN_POINT_MAG_LINEAR_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR)==unsigned(D3D11_FILTER_COMPARISON_MIN_POINT_MAG_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_LINEAR_MAG_MIP_POINT)==unsigned(D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR)==unsigned(D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_MAG_LINEAR_MIP_POINT)==unsigned(D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_MAG_MIP_LINEAR)==unsigned(D3D11_FILTER_MIN_MAG_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_MAG_MIP_POINT)==unsigned(D3D11_FILTER_MIN_MAG_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_MAG_POINT_MIP_LINEAR)==unsigned(D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT)==unsigned(D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT));
static_assert(unsigned(D3D10_DDI_FILTER_MIN_POINT_MAG_MIP_LINEAR)==unsigned(D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_TYPE_LINEAR)==unsigned(D3D11_FILTER_TYPE_LINEAR));
static_assert(unsigned(D3D10_DDI_FILTER_TYPE_MASK)==unsigned(D3D11_FILTER_TYPE_MASK));
static_assert(unsigned(D3D10_DDI_FILTER_TYPE_POINT)==unsigned(D3D11_FILTER_TYPE_POINT));
static_assert(unsigned(D3D10_DDI_TEXTURE_ADDRESS_BORDER)==unsigned(D3D11_TEXTURE_ADDRESS_BORDER));
static_assert(unsigned(D3D10_DDI_TEXTURE_ADDRESS_CLAMP)==unsigned(D3D11_TEXTURE_ADDRESS_CLAMP));
static_assert(unsigned(D3D10_DDI_TEXTURE_ADDRESS_MIRROR)==unsigned(D3D11_TEXTURE_ADDRESS_MIRROR));
static_assert(unsigned(D3D10_DDI_TEXTURE_ADDRESS_MIRRORONCE)==unsigned(D3D11_TEXTURE_ADDRESS_MIRROR_ONCE));
static_assert(unsigned(D3D10_DDI_TEXTURE_ADDRESS_WRAP)==unsigned(D3D11_TEXTURE_ADDRESS_WRAP));
D3D11_SAMPLER_DESC convert_sampler(const D3D10_DDI_SAMPLER_DESC &s) {
    D3D11_SAMPLER_DESC out{};
    out.Filter=static_cast<D3D11_FILTER>(s.Filter);
    out.AddressU=static_cast<D3D11_TEXTURE_ADDRESS_MODE>(s.AddressU);
    out.AddressV=static_cast<D3D11_TEXTURE_ADDRESS_MODE>(s.AddressV);
    out.AddressW=static_cast<D3D11_TEXTURE_ADDRESS_MODE>(s.AddressW);
    out.MipLODBias=s.MipLODBias; out.MaxAnisotropy=s.MaxAnisotropy;
    out.ComparisonFunc=static_cast<D3D11_COMPARISON_FUNC>(s.ComparisonFunc);
    for (unsigned i=0;i<4;++i) out.BorderColor[i]=s.BorderColor[i];
    out.MinLOD=s.MinLOD; out.MaxLOD=s.MaxLOD; return out;
}
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D10_DDI_SAMPLER_DESC *) { return sizeof(DdiSampler); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D10_DDI_SAMPLER_DESC *desc,
    D3D10DDI_HSAMPLER sampler,D3D10DDI_HRTSAMPLER) {
    auto *storage=static_cast<DdiSampler *>(sampler.pDrvPrivate);
    if (storage) storage->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!storage || !desc || !owner.device()) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return; }
        if (desc->Filter==D3D10_DDI_FILTER_TEXT_1BIT) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return; }
        const auto converted=convert_sampler(*desc);
        HRESULT hr=owner.device()->CreateSamplerState(&converted,&storage->object);
        if (FAILED(hr)) {
            if (storage->object) { storage->object->Release(); storage->object=nullptr; }
            report_ddi_error(owner,hr,DdiErrorClass::out_of_memory);
        } else if (!storage->object) report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory);
    },DdiErrorClass::out_of_memory);
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HSAMPLER sampler) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiSampler *>(sampler.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
template<auto Set> void APIENTRY bind(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HSAMPLER *samplers) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        constexpr UINT limit=D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
        if (first>=limit || count>limit-first || (count && !samplers)) {
            report_ddi_error(*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner,D3DDDIERR_DEVICEREMOVED); return;
        }
        std::array<ID3D11SamplerState *,limit> states{};
        for (UINT i=0;i<count;++i) {
            auto *s=static_cast<DdiSampler *>(samplers[i].pDrvPrivate);
            states[i]=s ? s->object : nullptr;
        }
        (context.*Set)(first,count,states.data());
    });
}
}
void install_sampler_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateSamplerSize=size; t.pfnCreateSampler=create; t.pfnDestroySampler=destroy;
    t.pfnVsSetSamplers=bind<&ID3D11DeviceContext4::VSSetSamplers>;
    t.pfnGsSetSamplers=bind<&ID3D11DeviceContext4::GSSetSamplers>;
    t.pfnPsSetSamplers=bind<&ID3D11DeviceContext4::PSSetSamplers>;
    t.pfnHsSetSamplers=bind<&ID3D11DeviceContext4::HSSetSamplers>;
    t.pfnDsSetSamplers=bind<&ID3D11DeviceContext4::DSSetSamplers>;
    t.pfnCsSetSamplers=bind<&ID3D11DeviceContext4::CSSetSamplers>;
}
}
