// SPDX-License-Identifier: MIT
#include "ddi-draw.h"
#include "ddi-entry.h"
namespace bc250::umd {
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
}
}
