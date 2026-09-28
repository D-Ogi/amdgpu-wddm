// SPDX-License-Identifier: MIT
#include "ddi-present.h"
namespace bc250::umd {
namespace {
struct Submission { IBc250DxvkDevice *engine; ID3D11Resource *resource; UINT subresource; };
HRESULT submit(void *data) {
    auto &s=*static_cast<Submission *>(data);
    // E3: flush the frame and process deferred optimized pipeline work. Plain
    // context.Flush is insufficient for the engine's frame lifecycle.
    return s.engine->SubmitForPresent(s.resource,s.subresource);
}
HRESULT APIENTRY rotate(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *args) {
    if (!args) return E_INVALIDARG;
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(args->hDevice);
    if (!storage || !storage->owner) return E_INVALIDARG;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    try {
        if (!owner.engine()) return E_FAIL;
        return rotate_present_resources(args->pResources,args->Resources,[&](ID3D11Resource *const *resources,UINT count) {
            return owner.engine()->RotateResourceIdentities(resources,count);
        });
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
HRESULT APIENTRY present(DXGI_DDI_ARG_PRESENT *args) {
    if (!args) return E_INVALIDARG;
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(args->hDevice);
    if (!storage || !storage->owner) return E_INVALIDARG;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    try {
        if (!owner.engine()) return E_FAIL;
        auto *source=reinterpret_cast<DdiResource *>(args->hSurfaceToPresent);
        auto *destination=reinterpret_cast<DdiResource *>(args->hDstResource);
        // Only an importer may establish the allocation/subresource identity.
        // Ordinary engine-owned resources cannot be presented via this callback.
        if (!source || !source->object || !source->present_allocation ||
            source->present_subresource!=args->SrcSubResourceIndex) return E_INVALIDARG;
        if (destination && (!destination->object || !destination->present_allocation ||
            destination->present_subresource!=args->DstSubResourceIndex)) return E_INVALIDARG;
        Submission submission{owner.engine(),source->object,args->SrcSubResourceIndex};
        return present_runtime(owner.bridge(),source->present_allocation,
            destination ? destination->present_allocation : 0,args->pDXGIContext,submit,&submission);
    } catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
}
void install_present_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &t) { t.pfnPresent=present; t.pfnRotateResourceIdentities=rotate; }
}
