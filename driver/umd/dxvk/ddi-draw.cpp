// SPDX-License-Identifier: MIT
#include "ddi-draw.h"
#include "ddi-buffer-binding.h"
namespace bc250::umd {
bool valid_indirect_arguments(const D3D11_BUFFER_DESC &desc,UINT offset,UINT bytes) {
    return (desc.MiscFlags&D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS) && bytes &&
        offset%sizeof(UINT)==0 && offset<=desc.ByteWidth && bytes<=desc.ByteWidth-offset;
}
namespace {
void APIENTRY draw(D3D10DDI_HDEVICE h,UINT count,UINT first) {
    enter_context(h,[&](ID3D11DeviceContext4 &c){ c.Draw(count,first); });
}
void APIENTRY draw_indexed(D3D10DDI_HDEVICE h,UINT count,UINT first,INT base) {
    enter_context(h,[&](ID3D11DeviceContext4 &c){ c.DrawIndexed(count,first,base); });
}
void APIENTRY draw_instanced(D3D10DDI_HDEVICE h,UINT vertices,UINT instances,UINT first,UINT first_instance) {
    enter_context(h,[&](ID3D11DeviceContext4 &c){ c.DrawInstanced(vertices,instances,first,first_instance); });
}
void APIENTRY draw_indexed_instanced(D3D10DDI_HDEVICE h,UINT indices,UINT instances,UINT first,INT base,UINT first_instance) {
    enter_context(h,[&](ID3D11DeviceContext4 &c){ c.DrawIndexedInstanced(indices,instances,first,base,first_instance); });
}
void APIENTRY draw_auto(D3D10DDI_HDEVICE h) {
    enter_context(h,[](ID3D11DeviceContext4 &c){ c.DrawAuto(); });
}
template<auto Call,UINT Bytes> void APIENTRY indirect(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle,UINT offset) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        ID3D11Buffer *buffer=nullptr;
        if (FAILED(resource_buffer(handle,buffer)) || !buffer) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED); return; }
        D3D11_BUFFER_DESC desc{}; buffer->GetDesc(&desc);
        if (!valid_indirect_arguments(desc,offset,Bytes)) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED); return; }
        // Argument contents stay on the GPU. No Map or CPU readback here.
        (context.*Call)(buffer,offset);
    });
}
void APIENTRY dispatch(D3D10DDI_HDEVICE h,UINT x,UINT y,UINT z) {
    enter_context(h,[&](ID3D11DeviceContext4 &c){ c.Dispatch(x,y,z); });
}
}
void install_draw_ddi(D3D11_1DDI_DEVICEFUNCS &table) {
    table.pfnDraw=draw;
    table.pfnDrawIndexed=draw_indexed;
    table.pfnDrawInstanced=draw_instanced;
    table.pfnDrawIndexedInstanced=draw_indexed_instanced;
    table.pfnDrawAuto=draw_auto;
    table.pfnDispatch=dispatch;
    table.pfnDrawInstancedIndirect=indirect<&ID3D11DeviceContext4::DrawInstancedIndirect,sizeof(D3D11_DRAW_INSTANCED_INDIRECT_ARGS)>;
    table.pfnDrawIndexedInstancedIndirect=indirect<&ID3D11DeviceContext4::DrawIndexedInstancedIndirect,sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)>;
    // Dispatch has three UINT thread-group counts (X, Y, Z).
    table.pfnDispatchIndirect=indirect<&ID3D11DeviceContext4::DispatchIndirect,3*sizeof(UINT)>;
}
}
