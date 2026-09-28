// SPDX-License-Identifier: MIT
// engine-ddi: root signatures (D54-D56). The DDI hands over the parsed description; the engine takes a serialized
// blob, so engine-ddi serializes it itself (the system d3d12.dll is the runtime that loaded this driver, and the
// engine exports no serializer).
//
// Layout of the RTS0 part, version 1.1, as the embedded root signature of a DXC-compiled shader has it (the
// harness compares the bytes with one; for other shapes the offsets are explicit, so a reader follows them):
//   header (24 bytes): Version, NumParameters, ParametersOffset, NumStaticSamplers, StaticSamplersOffset, Flags;
//   parameter headers (12 bytes each): ParameterType, ShaderVisibility, PayloadOffset;
//   payloads in parameter order: a table is {NumRanges, RangesOffset} followed at once by its 24-byte ranges,
//   constants and root descriptors are 12 bytes;
//   static samplers (52 bytes each) at the end; StaticSamplersOffset is the end when there are none.
// The part is wrapped in a DXBC container with a zero checksum. The engine does not check it (vkd3d-proton,
// libs/vkd3d-shader/dxbc.c, parse_dxbc: "Ignoring DXBC checksum"); a checksum is needed before any other reader.
#include "internal.h"
#include <cstring>

namespace engine_ddi {

static_assert(D3D12DDI_ROOT_SIGNATURE_VERSION_1_1 == static_cast<int>(D3D_ROOT_SIGNATURE_VERSION_1_1),
              "root signature version");
static_assert(sizeof(D3D12DDI_DESCRIPTOR_RANGE_0013) == 24 && sizeof(D3D12DDI_ROOT_CONSTANTS) == 12 &&
              sizeof(D3D12DDI_ROOT_DESCRIPTOR_0013) == 12 && sizeof(D3D12DDI_STATIC_SAMPLER) == 52,
              "RTS0 1.1 payload sizes equal the DDI structures");

namespace {
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

void put(std::vector<uint8_t>& out, size_t offset, uint32_t value) noexcept {
    std::memcpy(out.data() + offset, &value, sizeof(value));
}
} // namespace

HRESULT serialize_root_signature(const D3D12DDI_ROOT_SIGNATURE_0013* rs, std::vector<uint8_t>& out) noexcept {
    if (!rs || (rs->NumParameters && !rs->pRootParameters) || (rs->NumStaticSamplers && !rs->pStaticSamplers))
        return E_INVALIDARG;
    // Size first, in 64 bits: every count comes from the caller.
    uint64_t size = 24 + uint64_t{12} * rs->NumParameters;
    for (UINT i = 0; i < rs->NumParameters; ++i) {
        const D3D12DDI_ROOT_PARAMETER_0013& p = rs->pRootParameters[i];
        switch (p.ParameterType) {
        case D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            if (p.DescriptorTable.NumDescriptorRanges && !p.DescriptorTable.pDescriptorRanges) return E_INVALIDARG;
            size += 8 + uint64_t{24} * p.DescriptorTable.NumDescriptorRanges;
            break;
        case D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
        case D3D12DDI_ROOT_PARAMETER_TYPE_CBV:
        case D3D12DDI_ROOT_PARAMETER_TYPE_SRV:
        case D3D12DDI_ROOT_PARAMETER_TYPE_UAV:
            size += 12;
            break;
        default:
            return E_INVALIDARG;
        }
    }
    const uint64_t samplers_offset = size;
    size += uint64_t{52} * rs->NumStaticSamplers;
    constexpr uint64_t kContainer = 32 + 4 + 8;         // header, one part offset, part header
    if (size + kContainer > UINT32_MAX) return E_INVALIDARG;

    try {
        out.assign(static_cast<size_t>(size + kContainer), 0);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    const size_t base = kContainer;                     // RTS0 data starts here
    put(out, 0, fourcc('D', 'X', 'B', 'C'));
    put(out, 20, 1);                                    // container version
    put(out, 24, static_cast<uint32_t>(out.size()));
    put(out, 28, 1);                                    // one part
    put(out, 32, 36);                                   // its offset
    put(out, 36, fourcc('R', 'T', 'S', '0'));
    put(out, 40, static_cast<uint32_t>(size));

    put(out, base + 0, D3D_ROOT_SIGNATURE_VERSION_1_1);
    put(out, base + 4, rs->NumParameters);
    put(out, base + 8, 24);
    put(out, base + 12, rs->NumStaticSamplers);
    put(out, base + 16, static_cast<uint32_t>(samplers_offset));
    put(out, base + 20, static_cast<uint32_t>(rs->Flags));
    uint32_t payload = 24 + 12 * rs->NumParameters;
    for (UINT i = 0; i < rs->NumParameters; ++i) {
        const D3D12DDI_ROOT_PARAMETER_0013& p = rs->pRootParameters[i];
        const size_t header = base + 24 + size_t{12} * i;
        put(out, header + 0, static_cast<uint32_t>(p.ParameterType));
        put(out, header + 4, static_cast<uint32_t>(p.ShaderVisibility));
        put(out, header + 8, payload);
        switch (p.ParameterType) {
        case D3D12DDI_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            const UINT n = p.DescriptorTable.NumDescriptorRanges;
            put(out, base + payload, n);
            put(out, base + payload + 4, payload + 8);
            if (n) std::memcpy(out.data() + base + payload + 8, p.DescriptorTable.pDescriptorRanges, size_t{24} * n);
            payload += 8 + 24 * n;
            break;
        }
        case D3D12DDI_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            std::memcpy(out.data() + base + payload, &p.Constants, 12);
            payload += 12;
            break;
        default:
            std::memcpy(out.data() + base + payload, &p.Descriptor, 12);
            payload += 12;
            break;
        }
    }
    if (rs->NumStaticSamplers)
        std::memcpy(out.data() + base + samplers_offset, rs->pStaticSamplers, size_t{52} * rs->NumStaticSamplers);
    return S_OK;
}

namespace {
SIZE_T APIENTRY calc_root_signature(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013*) {
    return sizeof(RootSignatureRecord);
}

HRESULT APIENTRY create_root_signature(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATE_ROOT_SIGNATURE_0013* args,
                                       D3D12DDI_HROOTSIGNATURE h) {
    DeviceContext* c = resolve(device);
    if (!c || !args || !h.pDrvPrivate || args->NodeMask > 1) return E_INVALIDARG;
    if (args->Version != D3D12DDI_ROOT_SIGNATURE_VERSION_1_1) return E_NOTIMPL;   // 1.2: sampler flags, later
    std::vector<uint8_t> blob;
    HRESULT hr = serialize_root_signature(args->pRootSignature_1_1, blob);
    if (FAILED(hr)) return hr;
    ID3D12RootSignature* rs = nullptr;
    hr = c->device->CreateRootSignature(0, blob.data(), blob.size(), __uuidof(ID3D12RootSignature),
                                        reinterpret_cast<void**>(&rs));
    if (FAILED(hr)) return hr;
    new (h.pDrvPrivate) RootSignatureRecord{{Tag::RootSignature, 0, rs, c}, args->pRootSignature_1_1->NumParameters};
    c->live.fetch_add(1);
    return S_OK;
}

void APIENTRY destroy_root_signature(D3D12DDI_HDEVICE device, D3D12DDI_HROOTSIGNATURE h) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<RootSignatureRecord>(h.pDrvPrivate, Tag::RootSignature, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return;
    }
    release_engine(r->h);
    poison(r->h);
    c->live.fetch_sub(1);
}
} // namespace

void fill_core_root_signatures(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCalcPrivateRootSignatureSize = calc_root_signature;
    t->pfnCreateRootSignature = create_root_signature;
    t->pfnDestroyRootSignature = destroy_root_signature;
}

} // namespace engine_ddi
