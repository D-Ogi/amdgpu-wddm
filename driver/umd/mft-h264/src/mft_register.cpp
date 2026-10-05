#include "mft_register.h"
#include "mft_h264.h"
#include <string.h>

namespace bc250h264 {

namespace {

const MFT_REGISTER_TYPE_INFO kInputTypes[] = {
    { MFMediaType_Video, MFVideoFormat_NV12 },
    { MFMediaType_Video, MFVideoFormat_IYUV },
    { MFMediaType_Video, MFVideoFormat_ARGB32 },
};

const MFT_REGISTER_TYPE_INFO kOutputTypes[] = {
    { MFMediaType_Video, MFVideoFormat_H264 },
};

const wchar_t kName[] = L"BC-250 H.264 Encoder MFT";
const wchar_t kHardwareUrl[] = L"amdgpu_wddm://h264-encoder/0";
const wchar_t kVendorId[] = L"VEN_1002";

} // namespace

const MFT_REGISTER_TYPE_INFO* RegistrationInputTypes(uint32_t* count)
{
    if (count != nullptr) {
        *count = ARRAYSIZE(kInputTypes);
    }
    return kInputTypes;
}

const MFT_REGISTER_TYPE_INFO* RegistrationOutputTypes(uint32_t* count)
{
    if (count != nullptr) {
        *count = ARRAYSIZE(kOutputTypes);
    }
    return kOutputTypes;
}

const wchar_t* RegistrationName()
{
    return kName;
}

uint32_t RegistrationFlags()
{
    // HARDWARE is what makes a client that asks for hardware encoders see us at all; ASYNCMFT says
    // the object follows the asynchronous model. Both are required of a hardware MFT.
    return MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT;
}

HRESULT BuildRegistrationAttributes(IMFAttributes** out)
{
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 8);
    if (FAILED(hr)) {
        return hr;
    }
    hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC, 1);
    if (SUCCEEDED(hr)) { hr = attrs->SetUINT32(MF_SA_D3D11_AWARE, 1); }
    if (SUCCEEDED(hr)) { hr = attrs->SetUINT32(MF_SA_D3D_AWARE, 1); }
    if (SUCCEEDED(hr)) { hr = attrs->SetUINT32(MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE, 1); }
    if (SUCCEEDED(hr)) { hr = attrs->SetString(MFT_ENUM_HARDWARE_URL_Attribute, kHardwareUrl); }
    if (SUCCEEDED(hr)) { hr = attrs->SetString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, kVendorId); }
    if (FAILED(hr)) {
        return hr;
    }
    *out = attrs.Detach();
    return S_OK;
}

HRESULT BuildRegistrationBlob(RegBlob which, std::vector<uint8_t>& out)
{
    out.clear();
    switch (which) {
    case RegBlob::Attributes: {
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = BuildRegistrationAttributes(&attrs);
        if (FAILED(hr)) {
            return hr;
        }
        UINT32 size = 0;
        hr = MFGetAttributesAsBlobSize(attrs.Get(), &size);
        if (FAILED(hr)) {
            return hr;
        }
        out.resize(size);
        return MFGetAttributesAsBlob(attrs.Get(), out.data(), size);
    }
    case RegBlob::InputTypes:
        out.resize(sizeof(kInputTypes));
        memcpy(out.data(), kInputTypes, sizeof(kInputTypes));
        return S_OK;
    case RegBlob::OutputTypes:
        out.resize(sizeof(kOutputTypes));
        memcpy(out.data(), kOutputTypes, sizeof(kOutputTypes));
        return S_OK;
    case RegBlob::MftFlags: {
        const uint32_t flags = RegistrationFlags();
        out.resize(sizeof(flags));
        memcpy(out.data(), &flags, sizeof(flags));
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

HRESULT RegisterLocal(IClassFactory* factory)
{
    if (factory == nullptr) {
        return E_POINTER;
    }
    uint32_t inCount = 0, outCount = 0;
    const MFT_REGISTER_TYPE_INFO* in = RegistrationInputTypes(&inCount);
    const MFT_REGISTER_TYPE_INFO* outTypes = RegistrationOutputTypes(&outCount);
    return MFTRegisterLocal(factory, MFT_CATEGORY_VIDEO_ENCODER, kName, RegistrationFlags(),
                            inCount, in, outCount, outTypes);
}

HRESULT UnregisterLocal()
{
    return MFTUnregisterLocal(nullptr);
}

HRESULT RegisterGlobal(bool confirm)
{
    if (!confirm) {
        // A guard, not politeness: this writes under HKEY_LOCAL_MACHINE and must never happen by
        // accident on a development machine.
        return E_ACCESSDENIED;
    }
    uint32_t inCount = 0, outCount = 0;
    const MFT_REGISTER_TYPE_INFO* in = RegistrationInputTypes(&inCount);
    const MFT_REGISTER_TYPE_INFO* outTypes = RegistrationOutputTypes(&outCount);
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = BuildRegistrationAttributes(&attrs);
    if (FAILED(hr)) {
        return hr;
    }
    wchar_t name[64];
    wcscpy_s(name, kName);
    return MFTRegister(CLSID_Bc250H264EncoderMFT, MFT_CATEGORY_VIDEO_ENCODER, name,
                       RegistrationFlags(), inCount,
                       const_cast<MFT_REGISTER_TYPE_INFO*>(in), outCount,
                       const_cast<MFT_REGISTER_TYPE_INFO*>(outTypes), attrs.Get());
}

HRESULT UnregisterGlobal(bool confirm)
{
    if (!confirm) {
        return E_ACCESSDENIED;
    }
    return MFTUnregister(CLSID_Bc250H264EncoderMFT);
}

} // namespace bc250h264

extern "C" HRESULT __stdcall Bc250BuildMftRegistration(uint32_t which, uint8_t* buffer,
                                                       uint32_t capacity, uint32_t* needed)
{
    if (needed == nullptr) {
        return E_POINTER;
    }
    std::vector<uint8_t> blob;
    HRESULT hr = bc250h264::BuildRegistrationBlob(static_cast<bc250h264::RegBlob>(which), blob);
    if (FAILED(hr)) {
        return hr;
    }
    *needed = static_cast<uint32_t>(blob.size());
    if (buffer == nullptr || capacity < blob.size()) {
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    memcpy(buffer, blob.data(), blob.size());
    return S_OK;
}
