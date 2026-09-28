// SPDX-License-Identifier: MIT
#include "ddi-query.h"
#include <cstring>
namespace bc250::umd {
HRESULT convert_query(const D3D10DDIARG_CREATEQUERY &s,D3D11_QUERY_DESC &d,bool &predicate) {
    d={}; predicate=false;
    switch(s.Query) {
#define QUERY(a,b) case a: d.Query=b; break
    QUERY(D3D10DDI_QUERY_EVENT,D3D11_QUERY_EVENT);
    QUERY(D3D10DDI_QUERY_OCCLUSION,D3D11_QUERY_OCCLUSION);
    QUERY(D3D10DDI_QUERY_TIMESTAMP,D3D11_QUERY_TIMESTAMP);
    QUERY(D3D10DDI_QUERY_TIMESTAMPDISJOINT,D3D11_QUERY_TIMESTAMP_DISJOINT);
    QUERY(D3D10DDI_QUERY_PIPELINESTATS,D3D11_QUERY_PIPELINE_STATISTICS);
    QUERY(D3D11DDI_QUERY_PIPELINESTATS,D3D11_QUERY_PIPELINE_STATISTICS);
    QUERY(D3D10DDI_QUERY_OCCLUSIONPREDICATE,D3D11_QUERY_OCCLUSION_PREDICATE);
    QUERY(D3D10DDI_QUERY_STREAMOUTPUTSTATS,D3D11_QUERY_SO_STATISTICS);
    QUERY(D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE,D3D11_QUERY_SO_OVERFLOW_PREDICATE);
    QUERY(D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0,D3D11_QUERY_SO_STATISTICS_STREAM0);
    QUERY(D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM1,D3D11_QUERY_SO_STATISTICS_STREAM1);
    QUERY(D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM2,D3D11_QUERY_SO_STATISTICS_STREAM2);
    QUERY(D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3,D3D11_QUERY_SO_STATISTICS_STREAM3);
    QUERY(D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0);
    QUERY(D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM1,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1);
    QUERY(D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM2,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2);
    QUERY(D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3);
#undef QUERY
    default: return E_NOTIMPL;
    }
    predicate=d.Query==D3D11_QUERY_OCCLUSION_PREDICATE || d.Query==D3D11_QUERY_SO_OVERFLOW_PREDICATE ||
        (d.Query>=D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 && d.Query<=D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3);
    if (s.MiscFlags & ~UINT(D3D10DDI_QUERY_MISCFLAG_PREDICATEHINT)) return E_INVALIDARG;
    if (s.MiscFlags) {
        if (d.Query!=D3D11_QUERY_OCCLUSION_PREDICATE) return E_INVALIDARG;
        d.MiscFlags=D3D11_QUERY_MISC_PREDICATEHINT;
    }
    return S_OK;
}
HRESULT query_ddi_status(HRESULT hr) { return hr==S_FALSE ? DXGI_DDI_ERR_WASSTILLDRAWING : ddi_device_status(hr); }
namespace {
DeviceOwner &owner(D3D10DDI_HDEVICE h) { return *static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner; }
DdiQuery *query(D3D10DDI_HQUERY h) { return static_cast<DdiQuery *>(h.pDrvPrivate); }
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATEQUERY *) { return sizeof(DdiQuery); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATEQUERY *desc,D3D10DDI_HQUERY q,D3D10DDI_HRTQUERY) {
    auto *s=query(q); if (s) *s={};
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &o=owner(h);
        if (!s || !desc || !o.device()) { report_ddi_error(o,E_INVALIDARG); return; }
        D3D11_QUERY_DESC d{}; bool predicate=false; HRESULT hr=convert_query(*desc,d,predicate);
        if (SUCCEEDED(hr)) {
            if (predicate) { hr=o.device()->CreatePredicate(&d,&s->predicate); s->object=s->predicate; }
            else hr=o.device()->CreateQuery(&d,&s->object);
        }
        if (FAILED(hr) || !s->object) {
            if (s->object) s->object->Release(); *s={};
            report_ddi_error(o,FAILED(hr) ? hr : E_FAIL); return;
        }
        // predicate is a borrowed typed alias of the single owned COM reference.
        s->legacy_pipeline=desc->Query==D3D10DDI_QUERY_PIPELINESTATS;
    });
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY q) {
    enter_context(h,[&](ID3D11DeviceContext4 &) { auto *s=query(q); if (s) { if (s->object) s->object->Release(); *s={}; } });
}
template<bool Begin> void APIENTRY issue(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY q) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *s=query(q); if (!s || !s->object) { report_ddi_error(owner(h),E_INVALIDARG); return; }
        if constexpr(Begin) c.Begin(s->object); else c.End(s->object);
    });
}
void APIENTRY data(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY q,void *out,UINT bytes,UINT flags) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *s=query(q); auto &o=owner(h);
        if (!s || !s->object || (flags & ~UINT(D3D10_DDI_GET_DATA_DO_NOT_FLUSH)) || (!out && bytes)) {
            report_ddi_error(o,E_INVALIDARG); return;
        }
        if (o.bridge().device_lost || o.bridge().submission_failed) { report_ddi_error(o,D3DDDIERR_DEVICEREMOVED); return; }
        if (o.device()) {
            const HRESULT deviceStatus=o.device()->GetDeviceRemovedReason();
            if (FAILED(deviceStatus)) { report_ddi_error(o,deviceStatus); return; }
        }
        const UINT f=flags ? D3D11_ASYNC_GETDATA_DONOTFLUSH : 0;
        HRESULT hr;
        if (s->legacy_pipeline && out) {
            if (bytes!=sizeof(D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS)) { report_ddi_error(o,E_INVALIDARG); return; }
            D3D11_QUERY_DATA_PIPELINE_STATISTICS v{};
            hr=c.GetData(s->object,&v,sizeof(v),f);
            if (hr==S_OK) {
                D3D10_DDI_QUERY_DATA_PIPELINE_STATISTICS legacy{v.IAVertices,v.IAPrimitives,v.VSInvocations,
                    v.GSInvocations,v.GSPrimitives,v.CInvocations,v.CPrimitives,v.PSInvocations};
                std::memcpy(out,&legacy,sizeof(legacy));
            }
        } else hr=read_query_result(out,bytes,[&](void *buffer,UINT length) { return c.GetData(s->object,buffer,length,f); });
        // DDI has a void return: COM S_FALSE must become the DDI pending status.
        if (hr!=S_OK) report_ddi_error(o,query_ddi_status(hr));
    });
}
void APIENTRY predication(D3D10DDI_HDEVICE h,D3D10DDI_HQUERY q,BOOL value) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *s=query(q);
        if (s && !s->predicate) { report_ddi_error(owner(h),E_INVALIDARG); return; }
        c.SetPredication(s ? s->predicate : nullptr,value);
    });
}
}
void install_query_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateQuerySize=size; t.pfnCreateQuery=create; t.pfnDestroyQuery=destroy;
    t.pfnQueryBegin=issue<true>; t.pfnQueryEnd=issue<false>; t.pfnQueryGetData=data; t.pfnSetPredication=predication;
}
}
