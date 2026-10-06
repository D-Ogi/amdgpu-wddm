// SPDX-License-Identifier: MIT
#include "ddi-input-layout.h"
#include "engine-input-layout.h"
namespace bc250::umd {
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEELEMENTLAYOUT *) { return sizeof(DdiInputLayout); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATEELEMENTLAYOUT *desc,
    D3D10DDI_HELEMENTLAYOUT layout,D3D10DDI_HRTELEMENTLAYOUT) {
    auto *storage=static_cast<DdiInputLayout *>(layout.pDrvPrivate);
    if (storage) storage->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!storage || !desc) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return; }
        HRESULT hr=bc250_create_engine_input_layout(owner.engine(),*desc,&storage->object);
        if (FAILED(hr)) report_ddi_error(owner,hr,DdiErrorClass::out_of_memory);
    },DdiErrorClass::out_of_memory);
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HELEMENTLAYOUT layout) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *storage=static_cast<DdiInputLayout *>(layout.pDrvPrivate);
        if (storage && storage->object) { storage->object->Release(); storage->object=nullptr; }
    });
}
void APIENTRY bind(D3D10DDI_HDEVICE h,D3D10DDI_HELEMENTLAYOUT layout) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *storage=static_cast<DdiInputLayout *>(layout.pDrvPrivate);
        context.IASetInputLayout(storage ? storage->object : nullptr);
    });
}
}
void install_input_layout_ddi(D3D11_1DDI_DEVICEFUNCS &table) {
    table.pfnCalcPrivateElementLayoutSize=size;
    table.pfnCreateElementLayout=create;
    table.pfnDestroyElementLayout=destroy;
    table.pfnIaSetInputLayout=bind;
}
}
