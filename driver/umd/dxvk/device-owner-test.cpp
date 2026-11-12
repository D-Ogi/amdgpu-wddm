// SPDX-License-Identifier: MIT
#include "device-owner.h"
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <thread>
using namespace bc250::umd;
namespace {
void check(bool ok) { if (!ok) std::abort(); }
int identity, context; unsigned creates, destroys; bool fail_create=false,fail_destroy=false,lost_destroy=false;
// BD-327: on_destroy runs inside the destroy callback of close; locks counts the Lock2 calls of the workers.
unsigned locks; std::function<void()> on_destroy;
unsigned sync_destroys, paging_destroys; HRESULT sync_status=S_OK, paging_status=S_OK;
// BD-372: stand-ins for the WDDM 2.1 offer and reclaim callbacks; initialize keeps or clears them, nothing calls them.
HRESULT APIENTRY offer2(HANDLE,const D3DDDICB_OFFERALLOCATIONS2 *) { std::abort(); }
HRESULT APIENTRY reclaim3(HANDLE,D3DDDICB_RECLAIMALLOCATIONS3 *) { std::abort(); }
HRESULT APIENTRY create(HANDLE h,D3DDDICB_CREATECONTEXTVIRTUAL *c) {
    check(h==&identity && c->EngineAffinity==1); ++creates;
    if (fail_create) return E_OUTOFMEMORY;
    c->hContext=&context; return S_OK;
}
HRESULT APIENTRY destroy(HANDLE h,const D3DDDICB_DESTROYCONTEXT *c) {
    check(h==&identity && c->hContext==&context); ++destroys;
    if (on_destroy) on_destroy();
    return fail_destroy ? E_FAIL : lost_destroy ? D3DDDIERR_DEVICEREMOVED : S_OK;
}
HRESULT APIENTRY destroy_sync(HANDLE h,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *d) {
    check(h==&identity && d->hSyncObject==9); ++sync_destroys; return sync_status;
}
HRESULT APIENTRY destroy_paging(HANDLE h,const D3DDDI_DESTROYPAGINGQUEUE *d) {
    check(h==&identity && d->hPagingQueue==11); ++paging_destroys; return paging_status;
}
void APIENTRY set_error(D3D10DDI_HRTCORELAYER,HRESULT) { std::abort(); }
// BD-327: a call of a pipeline worker (Lock2, one of the admitted driver-internal calls), on a thread of its own that
// is bound to the gate of the owner's device.
HRESULT APIENTRY lock2(HANDLE h,D3DDDICB_LOCK2 *l) { check(h==&identity && l->hAllocation==5); ++locks; l->pData=&identity; return S_OK; }
int32_t worker_lock(DeviceOwner &owner) {
    int32_t status=-1;
    std::thread worker([&]() {
        EngineWorkerGate::bind(&owner.runtime().workers);
        D3DKMT_LOCK2 lock{}; lock.hAllocation=5;
        status=host_dispatch(&owner.bridge(),BC250_HOST_Lock2,&lock);
        EngineWorkerGate::bind(nullptr);
    });
    worker.join();
    return status;
}
}
int main() {
    D3DDDI_DEVICECALLBACKS cb{}; cb.pfnCreateContextVirtualCb=create;
    cb.pfnDestroyContextCb=destroy; cb.pfnDestroySynchronizationObjectCb=destroy_sync;
    cb.pfnDestroyPagingQueueCb=destroy_paging;
    D3D10DDI_CORELAYER_DEVICECALLBACKS um{}; um.pfnSetErrorCb=set_error;
    DXGI_DDI_BASE_CALLBACKS dxgi{};
    D3D10DDIARG_CREATEDEVICE args{}; args.pKTCallbacks=&cb; args.pUMCallbacks=&um;
    args.DXGIBaseDDI.pDXGIBaseCallbacks=&dxgi;
    args.hRTDevice.handle=reinterpret_cast<decltype(args.hRTDevice.handle)>(&identity);
    BC250_DXVK_ENGINE_FUNCS funcs{};
    BC250_DXVK_SHELL_SERVICES services{};
    DeviceOwner owner;
    RuntimeSurface *surface=nullptr;
    RuntimeSurface foreignSurface;
    RuntimeSurfaceRequest surfaceRequest{};
    D3D11_TEXTURE2D_DESC1 surfaceDesc{};
    check(owner.begin_surface(surfaceRequest,surfaceDesc,surface)==E_UNEXPECTED && !surface);
    check(owner.finish_surface(foreignSurface)==E_INVALIDARG);
    check(owner.close_surface(foreignSurface)==E_INVALIDARG);
    check(owner.surface_count()==0 && !owner.has_live_objects());
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    RuntimeDomain::Scope entry(owner.runtime().domain);
    check(owner.begin_surface(surfaceRequest,surfaceDesc,surface)==E_UNEXPECTED && !surface);
    check(owner.finish_surface(foreignSurface)==E_INVALIDARG && owner.close_surface(foreignSurface)==E_INVALIDARG);
    fail_create=true;
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_OUTOFMEMORY);
    check(creates==1 && destroys==0 && !owner.runtime().present_context);
    // BD-253: the step that failed, for the CreateDevice line, survives the rollback.
    check(!std::strcmp(owner.step(),"present-context"));
    fail_create=false;
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_INVALIDARG);
    check(creates==2 && destroys==1 && !owner.runtime().present_context);
    check(!std::strcmp(owner.step(),"modules"));
    fail_destroy=true;
    check(FAILED(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)));
    check(creates==3 && destroys==2 && owner.runtime().present_context==&context && owner.has_live_objects());
    check(owner.runtime().DXGICallbacks==&dxgi);
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    check(creates==3);
    fail_destroy=false;
    check(owner.close()==S_OK && destroys==3 && !owner.runtime().present_context && !owner.runtime().DXGICallbacks);
    check(owner.close()==S_OK && destroys==3 && !owner.has_live_objects());
    check(owner.retain_code_modules(reinterpret_cast<const void *>(&create),reinterpret_cast<const void *>(&destroy))==S_OK);
    check(owner.retained_module_count()==3 && owner.has_live_objects());
    check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED);
    owner.runtime().present_context=&context; fail_destroy=true;
    check(owner.close()==E_FAIL && owner.retained_module_count()==3);
    // BD-253: the step that kept the device, for the DestroyDevice line.
    check(!std::strcmp(owner.step(),"close-present-context"));
    fail_destroy=false;
    check(owner.close()==S_OK && owner.retained_module_count()==0 && !owner.has_live_objects());
    check(!std::strcmp(owner.step(),"none"));
    // BD-253: the drain proof is the kernel's alone. A fence at the value of a host record is no proof: the ICD
    // publishes a record only after its submission and its signal succeeded, so an accepted submission k can still
    // run while the record and the fence are at k-1. Each call reads the fences again: no answer is kept, and only
    // the first call with a timeout waits.
    {
        HostBridge b{}; b.device=&owner.runtime();
        UINT64 present=3, progress=4;
        // BD-198: a device that queued no GPU work needs no proof of the kernel and no wait. The line comes once.
        check(device_queued_no_work(b) && settle_device_work(b,2000) && b.drain_tried && !kernel_ended_device_work(b));
        check(settle_device_work(b,0));
        // One counted call, whatever its status, and the kernel's proof is needed again.
        b.drain_tried=false; b.gpu_work_calls=1;
        check(!device_queued_no_work(b) && !settle_device_work(b,0));
        b.present_cpu=&present; b.present_sync=9; b.present_value=3;
        check(!settle_device_work(b,0));
        b.progress[2]={3,7,4,&progress};
        check(!settle_device_work(b,0) && !b.drain_tried);
        progress=UINT64_MAX; check(settle_device_work(b,0) && kernel_ended_device_work(b) && !b.drain_tried);
        // A TRUE is not kept: a fence that does not read the lost value any more gives FALSE again.
        progress=4; check(!settle_device_work(b,0));
        // A FALSE is not kept: after the one wait ended without the proof, a later kernel error is seen.
        check(!settle_device_work(b,5) && b.drain_tried);
        const ULONGLONG start=GetTickCount64();
        check(!settle_device_work(b,2000) && GetTickCount64()-start<500);
        progress=UINT64_MAX; check(settle_device_work(b,0) && settle_device_work(b,2000));
        present=UINT64_MAX; b.progress[2]={}; check(settle_device_work(b,0));
    }
    // BD-253: close on a lost device. While the kernel has not ended the work of the device, a lost-device status of a
    // runtime callback still keeps the device. When the kernel ends it during the one wait of close, close completes
    // and the device is freed. The proof stays valid for the steps after close-session, which forgets the fences.
    {
        UINT64 fence=4;
        owner.runtime().present_context=&context; lost_destroy=true; owner.bridge().gpu_work_calls=1;
        owner.bridge().device_lost=true; owner.bridge().drain_tried=true; owner.bridge().progress[0]={1,7,5,&fence};
        const unsigned before=destroys;
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && destroys==before+1 && owner.runtime().present_context==&context);
        check(!std::strcmp(owner.step(),"close-present-context") && owner.has_live_objects());
        // A fence at the record's value is no proof either.
        owner.bridge().device_lost=true; owner.bridge().progress[0]={1,7,4,&fence};
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && destroys==before+2 && owner.has_live_objects());
        owner.bridge().drain_tried=false; owner.bridge().device_lost=true; owner.bridge().progress[0]={1,7,5,&fence};
        std::thread gpu([&]() { Sleep(50); *(volatile UINT64 *)&fence=UINT64_MAX; });
        const ULONGLONG start=GetTickCount64();
        const HRESULT closed=owner.close();
        const ULONGLONG waited=GetTickCount64()-start;
        gpu.join();
        check(closed==S_OK && waited>=30 && waited<2000 && destroys==before+3);
        check(!owner.runtime().present_context && !owner.has_live_objects() && !std::strcmp(owner.step(),"none"));
        // close-present-sync: the same rule for the destroy of the present fence. That fence's own lost value is a
        // proof too.
        UINT64 presentFence=2;
        lost_destroy=false; sync_status=D3DDDIERR_DEVICEREMOVED;
        owner.bridge().device_lost=true; owner.bridge().drain_tried=true; owner.bridge().gpu_work_calls=1;
        owner.bridge().present_sync=9; owner.bridge().present_cpu=&presentFence; owner.bridge().present_value=2;
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && sync_destroys==1 && owner.bridge().present_sync==9);
        check(!std::strcmp(owner.step(),"close-present-sync"));
        presentFence=UINT64_MAX;
        check(owner.close()==S_OK && sync_destroys==2 && !owner.has_live_objects());
        // close-paging-queue: the same rule for the paging queue of the surfaces. A failure that is not a loss keeps
        // the queue whatever the fences say.
        UINT64 pagingFence=0, progressFence=4;
        sync_status=S_OK; paging_status=D3DDDIERR_DEVICEREMOVED;
        owner.surface_queue()={11,12,&pagingFence};
        owner.bridge().device_lost=true; owner.bridge().drain_tried=true; owner.bridge().gpu_work_calls=1;
        owner.bridge().progress[0]={1,7,5,&progressFence};
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && paging_destroys==1 && owner.has_live_objects());
        check(!std::strcmp(owner.step(),"close-paging-queue"));
        progressFence=UINT64_MAX; paging_status=E_FAIL;
        check(owner.close()==E_FAIL && paging_destroys==2 && owner.has_live_objects());
        paging_status=D3DDDIERR_DEVICEREMOVED;
        check(owner.close()==S_OK && paging_destroys==3 && !owner.has_live_objects());
        paging_status=S_OK;
        // A failure that is not a loss still keeps the device, whatever the fences say.
        owner.runtime().present_context=&context; lost_destroy=false; fail_destroy=true;
        owner.bridge().device_lost=true;
        check(owner.close()==E_FAIL && owner.has_live_objects());
        fail_destroy=false;
        check(owner.close()==S_OK && !owner.has_live_objects());
    }
    // BD-198: a device lost before its first submission reached the kernel (Windows Terminal, the XAML of explorer) has
    // no progress record and no present fence, so the kernel's proof never comes. With no GPU work queued, close frees it
    // at once: no 2 s wait, no kept device. One counted call keeps the old rule.
    {
        owner.runtime().present_context=&context; lost_destroy=true;
        owner.bridge().device_lost=true;
        const unsigned before=destroys;
        const ULONGLONG start=GetTickCount64();
        check(owner.close()==S_OK && GetTickCount64()-start<500 && destroys==before+1);
        check(!owner.runtime().present_context && !owner.has_live_objects() && !std::strcmp(owner.step(),"none"));
        owner.runtime().present_context=&context;
        owner.bridge().device_lost=true; owner.bridge().drain_tried=true; owner.bridge().gpu_work_calls=1;
        check(owner.close()==D3DDDIERR_DEVICEREMOVED && destroys==before+2 && owner.has_live_objects());
        check(!std::strcmp(owner.step(),"close-present-context"));
        lost_destroy=false;
        check(owner.close()==S_OK && destroys==before+3 && !owner.has_live_objects());
    }
    // BD-327: the gate of the engine's pipeline workers. close closes it when it returns, on each path, and only an
    // initialize after a successful close opens it again. A successful close joined the workers in the engine's final
    // Release, so a worker call until then is admitted; a failed close keeps the engine and its workers, and their
    // later calls are refused. The tests open the gate as initialize does (no engine runs here).
    {
        RuntimeDevice &rt=owner.runtime();
        rt.KTCallbacks.pfnLock2Cb=lock2;
        check(rt.workers.revoked() && worker_lock(owner)==RuntimeDomain::wrong_domain && locks==0);
        // A successful close: a worker call inside close (here in its destroy callback) is admitted; after the
        // return it is refused.
        rt.workers.reopen(); rt.present_context=&context;
        int32_t inClose=-1; on_destroy=[&]() { inClose=worker_lock(owner); };
        check(owner.close()==S_OK && inClose==0 && locks==1);
        check(rt.workers.revoked() && worker_lock(owner)==RuntimeDomain::wrong_domain && locks==1);
        // An initialize after a successful close opens the gate again (it fails here at its argument check).
        D3D10DDIARG_CREATEDEVICE bad{};
        check(owner.initialize(bad,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_INVALIDARG && !rt.workers.revoked());
        rt.KTCallbacks.pfnLock2Cb=lock2;
        check(worker_lock(owner)==0 && locks==2);
        // A failed close before the engine's release (the paging queue of the surfaces): the engine and its workers
        // would stay, so the gate is closed when close returns, and their later calls are refused.
        UINT64 queueFence=0; paging_status=E_FAIL; owner.surface_queue()={11,12,&queueFence};
        inClose=-1; on_destroy=nullptr;
        check(owner.close()==E_FAIL && !std::strcmp(owner.step(),"close-paging-queue") && owner.has_live_objects());
        check(rt.workers.revoked() && worker_lock(owner)==RuntimeDomain::wrong_domain && locks==2);
        // No reopen after a failed close: initialize refuses the kept owner.
        check(owner.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_UNEXPECTED && rt.workers.revoked());
        check(worker_lock(owner)==RuntimeDomain::wrong_domain && locks==2);
        // The retries of a failed close keep the gate closed: the workers of the kept engine are refused also while a
        // retry runs, until the retry that succeeds.
        paging_status=S_OK; rt.present_context=&context; fail_destroy=true;
        on_destroy=[&]() { inClose=worker_lock(owner); };
        check(owner.close()==E_FAIL && inClose==RuntimeDomain::wrong_domain && rt.workers.revoked());
        on_destroy=nullptr; fail_destroy=false;
        check(owner.close()==S_OK && !owner.has_live_objects() && rt.workers.revoked() && locks==2);
    }
    // BD-372: pfnOfferAllocations2Cb and pfnReclaimAllocations3Cb are kept on a WDDM 2.2 device only; on any other
    // interface the copy has them cleared. Each initialize stops at its first callback (fail_create), after the copy.
    {
        DeviceOwner second; RuntimeDomain::Scope scope(second.runtime().domain);
        cb.pfnOfferAllocations2Cb=offer2; cb.pfnReclaimAllocations3Cb=reclaim3; fail_create=true;
        const auto &copied=second.runtime().KTCallbacks;
        const UINT interfaces[][2]={{D3D11_1_DDI_INTERFACE_VERSION,D3D11_1_DDI_BUILD_VERSION<<16},
            {D3DWDDM2_0_DDI_INTERFACE_VERSION,D3DWDDM2_0_DDI_BUILD_VERSION<<16},
            {D3DWDDM2_2_DDI_INTERFACE_VERSION,(D3DWDDM2_2_DDI_BUILD_VERSION-1)<<16}};
        for (const auto &pair:interfaces) {
            args.Interface=pair[0]; args.Version=pair[1];
            check(second.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_OUTOFMEMORY);
            check(copied.pfnCreateContextVirtualCb==create && !copied.pfnOfferAllocations2Cb &&
                  !copied.pfnReclaimAllocations3Cb);
        }
        args.Interface=D3DWDDM2_2_DDI_INTERFACE_VERSION; args.Version=D3DWDDM2_2_DDI_BUILD_VERSION<<16;
        check(second.initialize(args,1,nullptr,funcs,D3D_FEATURE_LEVEL_11_0,services)==E_OUTOFMEMORY);
        check(copied.pfnOfferAllocations2Cb==offer2 && copied.pfnReclaimAllocations3Cb==reclaim3);
        fail_create=false;
    }
    std::cout << "PASS DDI device owner: runtime arguments, initialization rollback, failed cleanup retention/retry, "
        "close on a lost device, engine worker gate, WDDM 2.1 offer callbacks on the WDDM 2.2 device only\n";
}
