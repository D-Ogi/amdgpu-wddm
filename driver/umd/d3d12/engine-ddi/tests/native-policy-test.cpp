// SPDX-License-Identifier: MIT
// Linked against the native engine-ddi.lib (no harness macro), the way the shell's DLL links it. Checks what the
// native build must refuse, and that both tables come out with every slot filled. No engine is loaded: the refusals
// happen before engine-ddi touches the engine device, which is a dummy pointer here.
#include "engine-ddi.h"
#include "internal.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <io.h>

namespace {
int failures = 0;
void check(bool ok, const char* format, ...) {
    char what[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(what, sizeof(what), format, args);
    va_end(args);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
    failures += ok ? 0 : 1;
}

// The last HRESULT a void slot reported, for the fail-safe checks below. S_OK means nothing was reported.
HRESULT g_reported = S_OK;
void APIENTRY device_error(void*, HRESULT hr) { g_reported = hr; }
void APIENTRY list_error(void*, D3D12DDI_HRTCOMMANDLIST, HRESULT) {}
BOOL APIENTRY device_lost(void*) { return FALSE; }
HRESULT APIENTRY bind_table(void*, D3D12DDI_HRTCOMMANDLIST, uint32_t) { return S_OK; }
HRESULT APIENTRY create_queue(void*, const BC250_VKD3D_COMMAND_QUEUE_DESC*, void*, REFIID, void**) { return E_FAIL; }

// A context the fail-safe checks can report through. The checks that want an unresolvable device pass a handle
// with a null pDrvPrivate, which engine_ddi::resolve answers without asking the resolver at all; everything else
// resolves here. No engine call is made on it: the slots below refuse before they reach the engine device.
engine_ddi::DeviceContext g_context;
engine_ddi::DeviceContext* APIENTRY resolve(D3D12DDI_HDEVICE) { return &g_context; }

template <class T> bool all_filled(const T& table) {
    void* slots[sizeof(T) / sizeof(void*)];
    std::memcpy(slots, &table, sizeof(T));
    for (void* s : slots)
        if (!s) return false;
    return true;
}
} // namespace

int main() {
    // The refusals below log through engine-ddi's stderr sink by design: expected output, sent to stdout in order.
    setvbuf(stdout, nullptr, _IONBF, 0);
    _dup2(_fileno(stdout), _fileno(stderr));
    SetEnvironmentVariableA("AMDGPU_WDDM_LOG", "stderr"); // the sink is off by default (stdio-log.h)
    BC250_VKD3D_ENGINE_FUNCS funcs{};
    funcs.Size = sizeof(funcs);
    funcs.AbiVersion = BC250_VKD3D_ENGINE_ABI_VERSION;
    funcs.CreateCommandQueue = create_queue;
    engine_ddi::ContextCreateInfo info{};
    info.size = sizeof(info);
    info.boundary_revision = engine_ddi::kBoundaryRevision;
    info.memory_mode = engine_ddi::MemoryMode::EnginePrivateTest;
    info.engine_device = reinterpret_cast<ID3D12Device*>(uintptr_t{0x1000});   // never dereferenced
    info.engine_funcs = &funcs;
    info.hooks.size = sizeof(info.hooks);
    info.hooks.report_device_error = device_error;
    info.hooks.report_list_error = list_error;
    info.hooks.is_device_lost = device_lost;
    info.hooks.bind_list_table = bind_table;
    engine_ddi::DeviceContext* context = reinterpret_cast<engine_ddi::DeviceContext*>(uintptr_t{1});
    check(engine_ddi::create_device_context(&info, &context) == E_INVALIDARG && !context,
          "native build refuses MemoryMode::EnginePrivateTest");

    engine_ddi::FillInfo fill{sizeof(fill), resolve};
    D3D12DDI_DEVICE_FUNCS_CORE_0088 core{};
    D3D12DDI_COMMAND_LIST_FUNCS_3D_0092 lists[2]{};
    check(engine_ddi::fill_device_core(&core, sizeof(core) - 8, &fill) == E_INVALIDARG, "core table: wrong size refused");
    check(engine_ddi::fill_command_list(&lists[0], sizeof(lists[0]), 2, &fill) == E_INVALIDARG,
          "list table: index 2 refused");
    check(engine_ddi::fill_device_core(&core, sizeof(core), &fill) == S_OK, "core table filled");
    check(engine_ddi::fill_command_list(&lists[0], sizeof(lists[0]), 0, &fill) == S_OK &&
              engine_ddi::fill_command_list(&lists[1], sizeof(lists[1]), 1, &fill) == S_OK,
          "compute and graphics list tables filled");
    // The shell fills its own slots over these; engine-ddi leaves them null.
    D3D12DDI_DEVICE_FUNCS_CORE_0088 shell_view = core;
    shell_view.pfnCalcPrivateCommandQueueSize = reinterpret_cast<decltype(shell_view.pfnCalcPrivateCommandQueueSize)>(1);
    shell_view.pfnCreateCommandQueue = reinterpret_cast<decltype(shell_view.pfnCreateCommandQueue)>(1);
    shell_view.pfnDestroyCommandQueue = reinterpret_cast<decltype(shell_view.pfnDestroyCommandQueue)>(1);
    shell_view.pfnCalcPrivateFenceSize = reinterpret_cast<decltype(shell_view.pfnCalcPrivateFenceSize)>(1);
    shell_view.pfnCreateFence = reinterpret_cast<decltype(shell_view.pfnCreateFence)>(1);
    shell_view.pfnDestroyFence = reinterpret_cast<decltype(shell_view.pfnDestroyFence)>(1);
    shell_view.pfnMakeResident = reinterpret_cast<decltype(shell_view.pfnMakeResident)>(1);
    shell_view.pfnEvict = reinterpret_cast<decltype(shell_view.pfnEvict)>(1);
    shell_view.pfnOfferResources = reinterpret_cast<decltype(shell_view.pfnOfferResources)>(1);
    shell_view.pfnReclaimResources = reinterpret_cast<decltype(shell_view.pfnReclaimResources)>(1);
    shell_view.pfnGetImplicitPhysicalAdapterMask = reinterpret_cast<decltype(shell_view.pfnGetImplicitPhysicalAdapterMask)>(1);
    shell_view.pfnGetPresentPrivateDriverDataSize = reinterpret_cast<decltype(shell_view.pfnGetPresentPrivateDriverDataSize)>(1);
    shell_view.pfnQueryNodeMap = reinterpret_cast<decltype(shell_view.pfnQueryNodeMap)>(1);
    shell_view.pfnGetDebugAllocationInfo = reinterpret_cast<decltype(shell_view.pfnGetDebugAllocationInfo)>(1);
    shell_view.pfnCalcPrivateSchedulingGroupSize = reinterpret_cast<decltype(shell_view.pfnCalcPrivateSchedulingGroupSize)>(1);
    shell_view.pfnCreateSchedulingGroup = reinterpret_cast<decltype(shell_view.pfnCreateSchedulingGroup)>(1);
    shell_view.pfnDestroySchedulingGroup = reinterpret_cast<decltype(shell_view.pfnDestroySchedulingGroup)>(1);
    shell_view.pfnSetBackgroundProcessingMode = reinterpret_cast<decltype(shell_view.pfnSetBackgroundProcessingMode)>(1);
    check(all_filled(shell_view), "core table: every engine-ddi slot non-null (the 18 shell slots aside)");
    check(!core.pfnCreateCommandQueue && !core.pfnCreateFence && !core.pfnMakeResident,
          "core table: shell slots left untouched");
    check(all_filled(lists[0]) && all_filled(lists[1]), "list tables: all 70 slots non-null");
    // The render pass table has two slots and no fail-safe form: both are filled or the table is not published.
    D3D12DDI_RENDER_PASS_FUNCS_0053 pass{};
    check(engine_ddi::fill_render_pass(nullptr, sizeof(pass), &fill) == E_INVALIDARG &&
              engine_ddi::fill_render_pass(&pass, sizeof(pass) - 8, &fill) == E_INVALIDARG && !all_filled(pass),
          "render pass table: a null table and a wrong size are refused with nothing written");
    check(engine_ddi::fill_render_pass(&pass, sizeof(pass), &fill) == S_OK && all_filled(pass),
          "render pass table: both slots non-null");

    // BD-075: the create and the open of a heap and resource may only report E_OUTOFMEMORY, so that a refusal
    // costs the application its object and not its device.
    using engine_ddi::admitted_create_failure;
    check(admitted_create_failure(S_OK) == S_OK && admitted_create_failure(S_FALSE) == S_FALSE,
          "admitted failure: a success is passed through");
    check(admitted_create_failure(E_OUTOFMEMORY) == E_OUTOFMEMORY &&
              admitted_create_failure(engine_ddi::kDriverDeviceRemoved) == engine_ddi::kDriverDeviceRemoved,
          "admitted failure: out of memory and D3DDDIERR_DEVICEREMOVED are passed through");
    // The two names of a lost device are not interchangeable here. DXGI_ERROR_DEVICE_REMOVED is what the API
    // shows the application and what create_heap_and_resource answers for a lost context (and heap-import for
    // VK_ERROR_DEVICE_LOST), but it is not in the AllowOutOfMemory list, so reporting it from a create slot is
    // the BD-075 signature over again, with DRIVER_INTERNAL_ERROR hiding the real reason.
    check(admitted_create_failure(DXGI_ERROR_DEVICE_REMOVED) == engine_ddi::kDriverDeviceRemoved &&
              admitted_create_failure(DXGI_ERROR_DEVICE_RESET) == engine_ddi::kDriverDeviceRemoved &&
              admitted_create_failure(DXGI_ERROR_DEVICE_HUNG) == engine_ddi::kDriverDeviceRemoved,
          "admitted failure: the DXGI device codes become D3DDDIERR_DEVICEREMOVED");
    check(admitted_create_failure(E_NOTIMPL) == E_OUTOFMEMORY &&
              admitted_create_failure(E_INVALIDARG) == E_OUTOFMEMORY &&
              admitted_create_failure(E_UNEXPECTED) == E_OUTOFMEMORY &&
              admitted_create_failure(E_FAIL) == E_OUTOFMEMORY,
          "admitted failure: every other failure becomes E_OUTOFMEMORY");

    // D64 and D65 are this module's slots now, not fail-safes. The device handle resolves to nothing here, so
    // the open refuses at its first argument check; a fail-safe would answer E_NOTIMPL and remove the device.
    const D3D12DDI_HDEVICE no_device{};
    const D3D12DDI_HHEAP no_heap{};
    const D3D12DDI_HRTRESOURCE no_rt{};
    const D3D12DDI_HRESOURCE no_resource{};
    const D3D12DDI_HPROTECTEDRESOURCESESSION_0030 no_session{};
    D3DDDI_OPENALLOCATIONINFO allocation{};
    D3D12DDIARG_OPENHEAP_0003 open{};
    open.NumAllocations = 1;
    open.pOpenAllocationInfo = &allocation;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES created =
        core.pfnCalcPrivateHeapAndResourceSizes(no_device, nullptr, nullptr, no_session);
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES opened =
        core.pfnCalcPrivateOpenedHeapAndResourceSizes(no_device, &open, no_session);
    check(opened.Heap == created.Heap && opened.Resource == created.Resource && opened.Resource > 64,
          "opened private sizes: the records a create builds, not a fail-safe's 64 bytes");
    check(core.pfnOpenHeapAndResource(no_device, &open, no_heap, no_rt, no_session, no_resource) == E_OUTOFMEMORY,
          "OpenHeapAndResource: refused with E_OUTOFMEMORY, the device survives");
    check(core.pfnOpenHeapAndResource(no_device, nullptr, no_heap, no_rt, no_session, no_resource) == E_OUTOFMEMORY,
          "OpenHeapAndResource: a missing argument is refused the same way");
    check(core.pfnCreateHeapAndResource(no_device, nullptr, no_heap, no_rt, nullptr, nullptr, no_session,
                                        no_resource) == E_OUTOFMEMORY,
          "CreateHeapAndResource: an unresolvable device is refused with E_OUTOFMEMORY");

    // BD-075 again, for the creation slots of the pipeline library (D86, D88, D90) and for the two that are still
    // fail-safes, D102 CreateMetaCommand and D117 CreateSamplerFeedbackUnorderedAccessView. Each must refuse the
    // way a creation that ran out of memory refuses, in its return value and, for the void one, in what it
    // reports. An E_NOTIMPL there is outside the runtime's AllowOutOfMemory category and costs the device. The
    // three library slots are implemented now (pipelines.cpp); every refusal they decide is clamped the same way,
    // and here they refuse a handle that names no live library of this device.
    g_context.hooks.size = sizeof(g_context.hooks);
    g_context.hooks.report_device_error = device_error;
    const D3D12DDI_HDEVICE device{&g_context};
    const D3D12DDI_HPIPELINELIBRARY no_library{};
    const D3D12DDI_HPIPELINESTATE no_pipeline{};
    check(core.pfnCreatePipelineLibrary(device, nullptr, no_library) == E_OUTOFMEMORY,
          "D86 CreatePipelineLibrary: a refused create answers E_OUTOFMEMORY, not E_NOTIMPL");
    check(core.pfnAddPipelineStateToLibrary(device, no_library, no_pipeline, 0) == E_OUTOFMEMORY,
          "D88 AddPipelineStateToLibrary: E_OUTOFMEMORY");
    check(core.pfnSerializeLibrary(device, no_library, nullptr) == E_OUTOFMEMORY, "D90 SerializeLibrary: E_OUTOFMEMORY");
    // D85 answers the size of the library record itself, not the header's worth of the fail-safe's calc, and D89
    // is a size query with no error channel: an unknown library is 0 and reports nothing at all.
    const D3D12DDIARG_CREATE_PIPELINE_LIBRARY_0010 empty_library{};
    g_reported = S_OK;
    const SIZE_T library_size = core.pfnCalcPrivatePipelineLibrarySize(device, &empty_library);
    const SIZE_T serialized = core.pfnCalcSerializedLibrarySize(device, no_library);
    check(library_size == sizeof(engine_ddi::PipelineLibraryRecord) && serialized == 0 && g_reported == S_OK,
          "D85/D89 pipeline library sizes: the library record of %zu bytes, and an unknown library serializes to "
          "0 with nothing reported (reported %08lx)", static_cast<size_t>(library_size),
          static_cast<unsigned long>(g_reported));
    const GUID no_command{};
    check(core.pfnCreateMetaCommand(device, no_command, 0, nullptr, 0, D3D12DDI_HMETACOMMAND_0052{},
                                    D3D12DDI_HRTMETACOMMAND_0052{}) == E_OUTOFMEMORY,
          "D102 CreateMetaCommand: E_OUTOFMEMORY");
    g_reported = S_OK;
    core.pfnCreateSamplerFeedbackUnorderedAccessView(device, no_resource, no_resource,
                                                     D3D12DDI_CPU_DESCRIPTOR_HANDLE{});
    check(g_reported == E_OUTOFMEMORY,
          "D117 CreateSamplerFeedbackUnorderedAccessView: a void create reports E_OUTOFMEMORY (reported %08lx)",
          static_cast<unsigned long>(g_reported));

    // D83 RetrieveShaderComment: the empty comment, which is one character, the terminator alone. Not a creation
    // slot, so a bad handle keeps E_INVALIDARG; the clamp of the category above did not spread to it.
    engine_ddi::PipelineRecord pipeline{};
    pipeline.h = engine_ddi::RecordHeader{engine_ddi::Tag::PipelineState, 0, nullptr, &g_context};
    const D3D12DDI_HPIPELINESTATE live_pipeline{&pipeline};
    WCHAR comment[4] = {L'x', L'x', L'x', L'x'};
    SIZE_T comment_chars = 4;
    const HRESULT wrote = core.pfnRetrieveShaderComment(device, live_pipeline, comment, &comment_chars);
    check(wrote == S_OK && comment_chars == 1 && comment[0] == L'\0' && comment[1] == L'x',
          "D83 RetrieveShaderComment: an empty comment is one character, written as the terminator alone and no "
          "further byte (hr %08lx, count %zu)", static_cast<unsigned long>(wrote),
          static_cast<size_t>(comment_chars));
    comment_chars = 0;
    const HRESULT queried = core.pfnRetrieveShaderComment(device, live_pipeline, nullptr, &comment_chars);
    const SIZE_T queried_chars = comment_chars;
    comment_chars = 0;
    const HRESULT no_room = core.pfnRetrieveShaderComment(device, live_pipeline, comment, &comment_chars);
    check(queried == S_OK && queried_chars == 1 && no_room == E_INVALIDARG && comment_chars == 1,
          "D83: a null buffer is the size query, and a buffer with no room for the terminator is refused with the "
          "needed count left in place (hr %08lx and %08lx)", static_cast<unsigned long>(queried),
          static_cast<unsigned long>(no_room));
    comment_chars = 4;
    check(core.pfnRetrieveShaderComment(device, no_pipeline, comment, &comment_chars) == E_INVALIDARG &&
              core.pfnRetrieveShaderComment(device, live_pipeline, comment, nullptr) == E_INVALIDARG,
          "D83: a handle that is no live pipeline of this device, and a missing count, are both E_INVALIDARG");

    // D76 CheckSubresourceInfo (resources.cpp): the two shapes whose memory layout is known here, and the third
    // shape, which is answered with zeros and no error code at all. An E_NOTIMPL from this slot would be the
    // BD-075 signature again: the reference allows this void query no error code, so the runtime would take one
    // as critical and remove the device.
    using engine_ddi::RecordHeader;
    using engine_ddi::ResourceRecord;
    using engine_ddi::Tag;
    ResourceRecord buffer{}, primary{}, image{};
    buffer.h = RecordHeader{Tag::Resource, 0, nullptr, &g_context};
    buffer.desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.desc.Width = 4096;
    primary.h = buffer.h;
    primary.desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    primary.desc.Width = 1920;
    primary.desc.Height = 1080;
    primary.desc.DepthOrArraySize = 1;
    primary.desc.MipLevels = 1;
    primary.desc.SampleDesc.Count = 1;
    primary.desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    primary.linear_row_pitch = 7936;                    // what the shell's linear surface query answered
    image = primary;
    image.linear_row_pitch = 0;                         // an ordinary image: no layout of its own is known
    image.desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    const auto subresource_info = [&](ResourceRecord* r, UINT index) {
        D3D12DDI_SUBRESOURCE_INFO out;
        std::memset(&out, 0xEE, sizeof(out));
        g_reported = S_OK;
        core.pfnCheckSubresourceInfo(device, D3D12DDI_HRESOURCE{r}, index, &out);
        return out;
    };
    {
        const D3D12DDI_SUBRESOURCE_INFO out = subresource_info(&buffer, 0);
        check(g_reported == S_OK && out.Offset == 0 && out.RowStride == 4096 && out.DepthStride == 4096 &&
                  !out.RowBytePreSwizzleOffset && !out.ColumnPreSwizzleOffset && !out.DepthPreSwizzleOffset,
              "D76 CheckSubresourceInfo: a buffer is one row of its width from its first byte, nothing swizzled "
              "(strides %llu and %llu, reported %08lx)", static_cast<unsigned long long>(out.RowStride),
              static_cast<unsigned long long>(out.DepthStride), static_cast<unsigned long>(g_reported));
    }
    {
        const D3D12DDI_SUBRESOURCE_INFO out = subresource_info(&primary, 0);
        check(g_reported == S_OK && out.Offset == 0 && out.RowStride == 7936 &&
                  out.DepthStride == uint64_t{7936} * 1080 && !out.RowBytePreSwizzleOffset &&
                  !out.ColumnPreSwizzleOffset && !out.DepthPreSwizzleOffset,
              "D76: the linear primary is its surface's row pitch and a plane of Height rows (strides %llu and "
              "%llu, reported %08lx)", static_cast<unsigned long long>(out.RowStride),
              static_cast<unsigned long long>(out.DepthStride), static_cast<unsigned long>(g_reported));
    }
    {
        const D3D12DDI_SUBRESOURCE_INFO out = subresource_info(&image, 0);
        check(g_reported == S_OK && out.Offset == 0 && out.RowStride == 0 && out.DepthStride == 0 &&
                  !out.RowBytePreSwizzleOffset && !out.ColumnPreSwizzleOffset && !out.DepthPreSwizzleOffset,
              "D76: an image with no known layout is answered with zeros and no error code (reported %08lx)",
              static_cast<unsigned long>(g_reported));
    }
    {
        const D3D12DDI_SUBRESOURCE_INFO out = subresource_info(&buffer, 1);
        check(g_reported == E_INVALIDARG && out.RowStride == 0 && out.DepthStride == 0,
              "D76: a subresource index the resource does not have is E_INVALIDARG with the answer zeroed "
              "(reported %08lx)", static_cast<unsigned long>(g_reported));
    }
    {
        const D3D12DDI_SUBRESOURCE_INFO out = subresource_info(nullptr, 0);
        check(g_reported == E_INVALIDARG && out.RowStride == 0,
              "D76: an unknown resource handle is E_INVALIDARG (reported %08lx)",
              static_cast<unsigned long>(g_reported));
    }
    g_reported = S_OK;
    core.pfnCheckSubresourceInfo(device, D3D12DDI_HRESOURCE{&buffer}, 0, nullptr);
    check(g_reported == E_INVALIDARG, "D76: no out structure is E_INVALIDARG (reported %08lx)",
          static_cast<unsigned long>(g_reported));

    // The initial barrier layout of a create (internal.h, legacy_initial_state; resources.cpp, initial_layout).
    // The five DDI-only layouts name a legacy state, every value the API has as well is passed to the engine as
    // it is (S_FALSE, and the create's cast to D3D12_BARRIER_LAYOUT is then an identity), and a value the DDI does
    // not define is refused. The DDI-only GENERIC_READ layout is the one that is not a 0x80000000 special, and
    // before this check it was taken for an API value and cast into the engine's create as 31.
    {
        using engine_ddi::legacy_initial_state;
        struct Classified {
            HRESULT first;
            D3D12_RESOURCE_STATES second;
        };
        const auto classify = [](D3D12DDI_BARRIER_LAYOUT layout) {
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
            const HRESULT hr = legacy_initial_state(layout, &state);
            return Classified{hr, state};
        };
        const auto generic = classify(D3D12DDI_BARRIER_LAYOUT_LEGACY_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE);
        check(generic.first == S_OK && generic.second == D3D12_RESOURCE_STATE_GENERIC_READ,
              "initial layout: the DDI's own GENERIC_READ layout is the legacy state GENERIC_READ, not an API "
              "layout (hr %08lx, state %08x)", static_cast<unsigned long>(generic.first),
              static_cast<unsigned>(generic.second));
        const auto copy_source = classify(D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_SOURCE);
        const auto copy_dest = classify(D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_DEST);
        const auto shader = classify(D3D12DDI_BARRIER_LAYOUT_LEGACY_SHADER_RESOURCE);
        const auto pixel = classify(D3D12DDI_BARRIER_LAYOUT_LEGACY_PIXEL_SHADER_RESOURCE);
        check(copy_source.first == S_OK && copy_source.second == D3D12_RESOURCE_STATE_COPY_SOURCE &&
                  copy_dest.first == S_OK && copy_dest.second == D3D12_RESOURCE_STATE_COPY_DEST &&
                  shader.first == S_OK &&
                  shader.second == (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
                  pixel.first == S_OK && pixel.second == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
              "initial layout: the four special layouts keep their legacy states");
        const auto undefined = classify(D3D12DDI_BARRIER_LAYOUT_UNDEFINED);
        const auto common = classify(D3D12DDI_BARRIER_LAYOUT_COMMON);
        const auto video = classify(D3D12DDI_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON);
        check(undefined.first == S_FALSE && common.first == S_FALSE && video.first == S_FALSE,
              "initial layout: UNDEFINED, COMMON and the last shared layout are the engine's to take as they are "
              "(hr %08lx, %08lx, %08lx)", static_cast<unsigned long>(undefined.first),
              static_cast<unsigned long>(common.first), static_cast<unsigned long>(video.first));
        const auto past = classify(static_cast<D3D12DDI_BARRIER_LAYOUT>(
            static_cast<unsigned>(D3D12DDI_BARRIER_LAYOUT_LEGACY_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE) + 1));
        const auto beyond = classify(static_cast<D3D12DDI_BARRIER_LAYOUT>(
            static_cast<unsigned>(D3D12DDI_BARRIER_LAYOUT_LEGACY_PIXEL_SHADER_RESOURCE) + 1));
        check(past.first == E_INVALIDARG && beyond.first == E_INVALIDARG,
              "initial layout: a value the DDI does not define is E_INVALIDARG on either side of the boundary "
              "(hr %08lx and %08lx)", static_cast<unsigned long>(past.first),
              static_cast<unsigned long>(beyond.first));
    }

    // Retire hand-off (set_retire_policy): the refusals that need no context, and the decision itself.
    engine_ddi::RetirePolicy policy{sizeof(policy), 1, 256, 250};
    check(engine_ddi::set_retire_policy(nullptr, &policy) == E_INVALIDARG, "retire policy: null context refused");
    using engine_ddi::retire_defers;
    check(!retire_defers(false, 5, 256, 1000, 990, 250), "retire policy: off never defers");
    check(!retire_defers(true, 0, 256, 1000, 990, 250), "retire policy: nothing pending, nothing to defer");
    check(retire_defers(true, 5, 256, 1000, 990, 250) && retire_defers(true, 255, 256, 1000, 751, 250),
          "retire policy: under both bounds a submission defers");
    check(!retire_defers(true, 256, 256, 1000, 990, 250) && !retire_defers(true, 4000, 256, 1000, 990, 250),
          "retire policy: the backlog bound makes a submission run the sequence");
    check(!retire_defers(true, 5, 256, 1000, 750, 250) && !retire_defers(true, 5, 256, 100000, 990, 250),
          "retire policy: the age bound makes a submission run the sequence");
    check(!retire_defers(true, 5, 256, 1000, 0, 250), "retire policy: before any resource DDI's pass it never defers");
    check(!retire_defers(true, 5, 256, 990, 1000, 250), "retire policy: a tick ahead of now does not defer");
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
