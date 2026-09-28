// SPDX-License-Identifier: MIT
#pragma once
#include "queue-request.h"
#include <new>
namespace native12 {
// Heap owners survive release of the runtime's QueueSlot after DestroyCommandQueue.
struct QueueOwner {
    QueueContext context;
    QueueOwner* next{};
    bool retired{};
    QueueOwner(D3D12DDI_HRTCOMMANDQUEUE h,const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb):context(h,cb) {}
};
struct QueueSlot {QueueOwner* owner{};};
class QueueRegistry final {
    SRWLOCK lock_=SRWLOCK_INIT;
    QueueOwner* head_{};
public:
    QueueRegistry()=default;
    QueueRegistry(const QueueRegistry&)=delete;
    QueueRegistry& operator=(const QueueRegistry&)=delete;
    HRESULT create(const D3D12DDIARG_CREATECOMMANDQUEUE_0050& desc,
                   D3D12DDI_HRTCOMMANDQUEUE runtime,
                   const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb,QueueSlot& slot) noexcept {
        if(slot.owner) return E_UNEXPECTED;
        ContextRequest request;HRESULT hr=request.prepare(desc);if(FAILED(hr))return hr;
        auto q=new(std::nothrow) QueueOwner(runtime,cb);if(!q)return E_OUTOFMEMORY;
        hr=q->context.open(request.args);if(FAILED(hr)){delete q;return hr;}
        AcquireSRWLockExclusive(&lock_);q->next=head_;head_=q;slot.owner=q;ReleaseSRWLockExclusive(&lock_);
        return S_OK;
    }
    HRESULT destroy(QueueSlot& slot) noexcept {
        auto q=slot.owner;if(!q)return S_OK;
        // Runtime serializes lifetime calls for one queue. Never hold the registry lock across callbacks.
        HRESULT hr=q->context.close();
        AcquireSRWLockExclusive(&lock_);slot.owner=nullptr;
        if(FAILED(hr)) q->retired=true;
        else {auto link=&head_;while(*link && *link!=q)link=&(*link)->next;if(*link)*link=q->next;}
        ReleaseSRWLockExclusive(&lock_);
        if(SUCCEEDED(hr))delete q;
        return hr;
    }
    // Called with device lifetime externally serialized, while callback scope is still valid.
    // Active queues prevent shutdown. Failed contexts stay owned for an explicit retry.
    HRESULT drain_retired() noexcept {
        AcquireSRWLockExclusive(&lock_);
        for(auto q=head_;q;q=q->next) if(!q->retired){ReleaseSRWLockExclusive(&lock_);return E_UNEXPECTED;}
        auto list=head_;head_=nullptr;ReleaseSRWLockExclusive(&lock_);
        HRESULT result=S_OK;
        while(list){auto q=list;list=q->next;HRESULT hr=q->context.close();
            if(SUCCEEDED(hr))delete q;
            else {result=hr;AcquireSRWLockExclusive(&lock_);q->next=head_;head_=q;ReleaseSRWLockExclusive(&lock_);}
        }
        return result;
    }
    bool empty() noexcept {
        AcquireSRWLockShared(&lock_);bool value=!head_;ReleaseSRWLockShared(&lock_);return value;
    }
};
}
