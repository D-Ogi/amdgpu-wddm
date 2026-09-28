// SPDX-License-Identifier: MIT
#include "ddi-shader.h"
#include <type_traits>
namespace bc250::umd {
HRESULT copy_legacy_signature(const D3D11_1DDIARG_SIGNATURE_ENTRY *entries,UINT count,
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> &out) {
    if (count && !entries) return E_INVALIDARG;
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> result(count);
    for (UINT i=0;i<count;++i) {
        const auto &s=entries[i];
        // Original D3D11.1 signature has no Stream; its padding is not data.
        result[i]={UINT(s.SystemValue),s.Register,s.Mask,0,UINT(s.RegisterComponentType),UINT(s.MinPrecision)};
    }
    out=std::move(result); return S_OK;
}
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const UINT *,const D3D11_1DDIARG_STAGE_IO_SIGNATURES *) { return sizeof(DdiShader); }
template<typename Shader,ShaderStage stage,typename Signature=D3D11_1DDIARG_STAGE_IO_SIGNATURES>
void APIENTRY create(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER shader,
    D3D10DDI_HRTSHADER,const Signature *signatures) {
    auto *storage=static_cast<DdiShader *>(shader.pDrvPrivate);
    if (storage) { storage->object=nullptr; storage->stage=stage; }
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!storage || !code || !signatures || !owner.engine()) { report_ddi_error(owner,E_INVALIDARG); return; }
        std::vector<BC250_DXVK_SIGNATURE_ENTRY> input,output,patch;
        HRESULT hr=copy_legacy_signature(signatures->pInputSignatureDeprecated,signatures->NumInputSignatureEntries,input);
        if (SUCCEEDED(hr)) hr=copy_legacy_signature(signatures->pOutputSignatureDeprecated,signatures->NumOutputSignatureEntries,output);
        if constexpr(std::is_same_v<Signature,D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES>) {
            if (SUCCEEDED(hr)) hr=copy_legacy_signature(signatures->pPatchConstantSignatureDeprecated,
                signatures->NumPatchConstantSignatureEntries,patch);
        }
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        BC250_DXVK_SHADER_DESC desc{}; desc.Size=sizeof(desc); desc.Code=code;
        desc.Input={input.data(),static_cast<UINT>(input.size())};
        desc.Output={output.data(),static_cast<UINT>(output.size())};
        desc.PatchConstant={patch.data(),static_cast<UINT>(patch.size())};
        Shader *object=nullptr;
        hr=owner.engine()->CreateShader(&desc,__uuidof(Shader),reinterpret_cast<void **>(&object));
        if (FAILED(hr)) { if (object) object->Release(); report_ddi_error(owner,hr); return; }
        if (!object) { report_ddi_error(owner,E_FAIL); return; }
        storage->object=object;
    });
}
SIZE_T APIENTRY tessellation_size(D3D10DDI_HDEVICE,const UINT *,const D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES *) {
    return sizeof(DdiShader);
}
void APIENTRY create_compute(D3D10DDI_HDEVICE h,const UINT *code,D3D10DDI_HSHADER shader,D3D10DDI_HRTSHADER runtime_shader) {
    const D3D11_1DDIARG_STAGE_IO_SIGNATURES empty{};
    create<ID3D11ComputeShader,ShaderStage::compute>(h,code,shader,runtime_shader,&empty);
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER shader) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiShader *>(shader.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
template<typename Shader,ShaderStage stage>
void APIENTRY bind(D3D10DDI_HDEVICE h,D3D10DDI_HSHADER shader) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *s=static_cast<DdiShader *>(shader.pDrvPrivate);
        if (s && s->stage!=stage) {
            report_ddi_error(*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner,E_INVALIDARG); return;
        }
        auto *object=s ? static_cast<Shader *>(s->object) : nullptr;
        if constexpr(stage==ShaderStage::vertex) context.VSSetShader(object,nullptr,0);
        if constexpr(stage==ShaderStage::geometry) context.GSSetShader(object,nullptr,0);
        if constexpr(stage==ShaderStage::pixel) context.PSSetShader(object,nullptr,0);
        if constexpr(stage==ShaderStage::compute) context.CSSetShader(object,nullptr,0);
        if constexpr(stage==ShaderStage::hull) context.HSSetShader(object,nullptr,0);
        if constexpr(stage==ShaderStage::domain) context.DSSetShader(object,nullptr,0);
    });
}
}
void install_shader_ddi(D3D11_1DDI_DEVICEFUNCS &table) {
    table.pfnCalcPrivateShaderSize=size;
    table.pfnCreateVertexShader=create<ID3D11VertexShader,ShaderStage::vertex>;
    table.pfnCreateGeometryShader=create<ID3D11GeometryShader,ShaderStage::geometry>;
    table.pfnCreatePixelShader=create<ID3D11PixelShader,ShaderStage::pixel>;
    table.pfnCreateComputeShader=create_compute;
    table.pfnCreateHullShader=create<ID3D11HullShader,ShaderStage::hull,D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES>;
    table.pfnCreateDomainShader=create<ID3D11DomainShader,ShaderStage::domain,D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES>;
    table.pfnCalcPrivateTessellationShaderSize=tessellation_size;
    table.pfnCsSetShader=bind<ID3D11ComputeShader,ShaderStage::compute>;
    table.pfnHsSetShader=bind<ID3D11HullShader,ShaderStage::hull>;
    table.pfnDsSetShader=bind<ID3D11DomainShader,ShaderStage::domain>;
    table.pfnDestroyShader=destroy;
    table.pfnVsSetShader=bind<ID3D11VertexShader,ShaderStage::vertex>;
    table.pfnGsSetShader=bind<ID3D11GeometryShader,ShaderStage::geometry>;
    table.pfnPsSetShader=bind<ID3D11PixelShader,ShaderStage::pixel>;
}
}
