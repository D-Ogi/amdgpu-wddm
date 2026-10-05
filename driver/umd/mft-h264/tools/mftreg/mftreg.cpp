// SPDX-License-Identifier: MIT
// Emits the registration data for the encoder transform, and - only with two explicit switches and
// only on the lab - performs a machine-wide registration.
//
// The point of this tool is that the bytes in the driver package's INF come out of the same code the
// transform itself uses, so the enumeration view and the live object cannot drift apart. Nothing
// here touches the registry unless both --register-global and the environment variable
// BC250_ALLOW_HKLM_MFT=1 are present; the default is to print.
//
//   mftreg.exe --inf        INF AddReg lines for the driver package
//   mftreg.exe --reg        the same as a .reg file, for a lab experiment
//   mftreg.exe --show       what the blobs contain, in readable form
//   mftreg.exe --enum       the H.264 encoder MFTs of this machine, and of each video adapter
//   mftreg.exe --register-global / --unregister-global   lab only, both guards required

#include "../../src/mft_register.h"
#include "../../src/mft_h264.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace bc250h264;

namespace {

struct MfStartup {
    HRESULT hr = E_FAIL;
    MfStartup()
    {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr)) {
            hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        }
    }
    ~MfStartup()
    {
        if (SUCCEEDED(hr)) {
            MFShutdown();
        }
        CoUninitialize();
    }
};

void PrintGuid(const GUID& g)
{
    printf("{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", g.Data1, g.Data2, g.Data3,
           g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
           g.Data4[7]);
}

// The class id and the category as the registry itself spells them under
// HKLM\SOFTWARE\Classes\MediaFoundation\Transforms: no braces. Measured on the development PC on
// 2026-10-05, where all 62 entries of that key and all 9 category keys are written this way,
// including the three the NVIDIA display driver package installs. COM's own
// HKLM\SOFTWARE\Classes\CLSID keys keep the braces, so the two shapes are printed by two functions.
void PrintBareGuid(const GUID& g)
{
    printf("%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3,
           g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
           g.Data4[7]);
}

void PrintTransformKey()
{
    printf("HKLM,\"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\");
    PrintBareGuid(CLSID_Bc250H264EncoderMFT);
    printf("\",");
}

void PrintInfBinary(const char* valueName, const std::vector<uint8_t>& bytes)
{
    // FLG_ADDREG_BINVALUETYPE is 0x00000001; the value is a comma separated list of hex bytes.
    PrintTransformKey();
    printf("\"%s\",0x00000001,", valueName);
    for (size_t i = 0; i < bytes.size(); ++i) {
        printf("%s%02x", (i == 0) ? "" : ",", bytes[i]);
        if ((i % 16) == 15 && i + 1 < bytes.size()) {
            printf(" \\\n    ");
        }
    }
    printf("\n");
}

void PrintRegBinary(const char* valueName, const std::vector<uint8_t>& bytes)
{
    printf("\"%s\"=hex:", valueName);
    for (size_t i = 0; i < bytes.size(); ++i) {
        printf("%s%02x", (i == 0) ? "" : ",", bytes[i]);
        if ((i % 20) == 19 && i + 1 < bytes.size()) {
            printf("\\\n  ");
        }
    }
    printf("\n");
}

int Fail(const char* what, HRESULT hr)
{
    fprintf(stderr, "%s failed 0x%08lX\n", what, static_cast<unsigned long>(hr));
    return 2;
}

int DoShow()
{
    uint32_t inCount = 0, outCount = 0;
    const MFT_REGISTER_TYPE_INFO* in = RegistrationInputTypes(&inCount);
    const MFT_REGISTER_TYPE_INFO* out = RegistrationOutputTypes(&outCount);
    printf("clsid       ");
    PrintGuid(CLSID_Bc250H264EncoderMFT);
    printf("\ncategory    ");
    PrintGuid(MFT_CATEGORY_VIDEO_ENCODER);
    printf("\nname        %ls\n", RegistrationName());
    printf("MFTFlags    0x%08x (HARDWARE|ASYNCMFT)\n", RegistrationFlags());
    for (uint32_t i = 0; i < inCount; ++i) {
        printf("input  %u    ", i);
        PrintGuid(in[i].guidMajorType);
        printf(" ");
        PrintGuid(in[i].guidSubtype);
        printf("\n");
    }
    for (uint32_t i = 0; i < outCount; ++i) {
        printf("output %u    ", i);
        PrintGuid(out[i].guidMajorType);
        printf(" ");
        PrintGuid(out[i].guidSubtype);
        printf("\n");
    }
    std::vector<uint8_t> attrs;
    HRESULT hr = BuildRegistrationBlob(RegBlob::Attributes, attrs);
    if (FAILED(hr)) {
        return Fail("BuildRegistrationBlob(Attributes)", hr);
    }
    printf("Attributes  %zu bytes\n", attrs.size());

    // Round trip: the blob has to come back as the same attribute store, or the registry value we are
    // about to print is not what the transform will be enumerated with.
    ComPtr<IMFAttributes> back;
    hr = MFCreateAttributes(&back, 8);
    if (SUCCEEDED(hr)) {
        hr = MFInitAttributesFromBlob(back.Get(), attrs.data(), static_cast<UINT32>(attrs.size()));
    }
    if (FAILED(hr)) {
        return Fail("MFInitAttributesFromBlob", hr);
    }
    UINT32 count = 0;
    back->GetCount(&count);
    printf("            round trip: %u attributes\n", count);
    for (UINT32 i = 0; i < count; ++i) {
        GUID key = GUID_NULL;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(back->GetItemByIndex(i, &key, &v))) {
            printf("            ");
            PrintGuid(key);
            if (v.vt == VT_UI4) {
                printf(" = %u\n", v.ulVal);
            } else if (v.vt == VT_LPWSTR) {
                printf(" = \"%ls\"\n", v.pwszVal);
            } else {
                printf(" (vt %u)\n", v.vt);
            }
        }
        PropVariantClear(&v);
    }
    return 0;
}

int DoInf()
{
    std::vector<uint8_t> attrs, inTypes, outTypes, flags;
    HRESULT hr = BuildRegistrationBlob(RegBlob::Attributes, attrs);
    if (SUCCEEDED(hr)) { hr = BuildRegistrationBlob(RegBlob::InputTypes, inTypes); }
    if (SUCCEEDED(hr)) { hr = BuildRegistrationBlob(RegBlob::OutputTypes, outTypes); }
    if (SUCCEEDED(hr)) { hr = BuildRegistrationBlob(RegBlob::MftFlags, flags); }
    if (FAILED(hr)) {
        return Fail("BuildRegistrationBlob", hr);
    }
    printf("; Generated by mftreg.exe --inf. Do not hand-edit: rebuild it from the sources instead.\n");
    printf("; AddReg lines for the machine-wide registration of the H.264 encoder MFT, which is\n");
    printf("; route A of INSTALL.md. Every key is named in full on purpose: HKR in a DDInstall\n");
    printf("; AddReg section is the device's own software key (install/inf-addreg-directive.md),\n");
    printf("; which is not where Media Foundation looks for a transform.\n");
    PrintTransformKey();
    printf(",0x00000000,\"%ls\"\n", RegistrationName());
    uint32_t f = 0;
    memcpy(&f, flags.data(), sizeof(f));
    PrintTransformKey();
    printf("\"MFTFlags\",0x00010001,0x%08x\n", f);
    PrintInfBinary("InputTypes", inTypes);
    PrintInfBinary("OutputTypes", outTypes);
    PrintInfBinary("Attributes", attrs);
    printf("; The category membership is the presence of the key, with no value in it:\n");
    printf("; FLG_ADDREG_KEYONLY (0x00000010) creates a key and ignores the value.\n");
    printf("HKLM,\"SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\");
    PrintBareGuid(MFT_CATEGORY_VIDEO_ENCODER);
    printf("\\");
    PrintBareGuid(CLSID_Bc250H264EncoderMFT);
    printf("\",,0x00000010,\n");
    return 0;
}

int DoReg()
{
    std::vector<uint8_t> attrs, inTypes, outTypes, flags;
    HRESULT hr = BuildRegistrationBlob(RegBlob::Attributes, attrs);
    if (SUCCEEDED(hr)) { hr = BuildRegistrationBlob(RegBlob::InputTypes, inTypes); }
    if (SUCCEEDED(hr)) { hr = BuildRegistrationBlob(RegBlob::OutputTypes, outTypes); }
    if (SUCCEEDED(hr)) { hr = BuildRegistrationBlob(RegBlob::MftFlags, flags); }
    if (FAILED(hr)) {
        return Fail("BuildRegistrationBlob", hr);
    }
    wchar_t clsid[64] = {};
    StringFromGUID2(CLSID_Bc250H264EncoderMFT, clsid, ARRAYSIZE(clsid));
    uint32_t f = 0;
    memcpy(&f, flags.data(), sizeof(f));

    printf("Windows Registry Editor Version 5.00\n\n");
    printf("[HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\MediaFoundation\\Transforms\\");
    PrintBareGuid(CLSID_Bc250H264EncoderMFT);
    printf("]\n");
    printf("@=\"%ls\"\n", RegistrationName());
    printf("\"MFTFlags\"=dword:%08x\n", f);
    PrintRegBinary("InputTypes", inTypes);
    PrintRegBinary("OutputTypes", outTypes);
    PrintRegBinary("Attributes", attrs);
    printf("\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\");
    PrintBareGuid(MFT_CATEGORY_VIDEO_ENCODER);
    printf("\\");
    PrintBareGuid(CLSID_Bc250H264EncoderMFT);
    printf("]\n");
    printf("\n; The COM server itself. In the driver package this is the InprocServer32 of the DLL that\n");
    printf("; the package installs; here it is spelled out so a lab experiment can point it at a\n");
    printf("; staged copy.\n");
    printf("[HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\%ls]\n", clsid);
    printf("@=\"%ls\"\n", RegistrationName());
    printf("\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\CLSID\\%ls\\InprocServer32]\n", clsid);
    printf("@=\"C:\\\\Windows\\\\System32\\\\amdgpu_wddm_mft_h264.dll\"\n");
    printf("\"ThreadingModel\"=\"Both\"\n");
    return 0;
}

// MFT_ENUM_ADAPTER_LUID (mfapi.h:2025) as the activation object carries it. The attribute's
// documented data type is LUID, that is an 8 byte blob, and the MFTEnum2 reference passes it with
// SetBlob(..., sizeof(LUID)); this prints whatever type is actually there, so a disagreement between
// the documentation and the platform is visible instead of guessed at.
void PrintAdapterLuid(IMFAttributes* attrs)
{
    MF_ATTRIBUTE_TYPE type = MF_ATTRIBUTE_UINT32;
    if (FAILED(attrs->GetItemType(MFT_ENUM_ADAPTER_LUID, &type))) {
        printf("     MFT_ENUM_ADAPTER_LUID absent\n");
        return;
    }
    if (type == MF_ATTRIBUTE_BLOB) {
        UINT32 size = 0;
        LUID luid = {};
        if (SUCCEEDED(attrs->GetBlobSize(MFT_ENUM_ADAPTER_LUID, &size)) && size == sizeof(luid) &&
            SUCCEEDED(attrs->GetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<UINT8*>(&luid),
                                     sizeof(luid), nullptr))) {
            printf("     MFT_ENUM_ADAPTER_LUID blob %u bytes, %08lx:%08lx\n", size,
                   static_cast<unsigned long>(luid.HighPart),
                   static_cast<unsigned long>(luid.LowPart));
        } else {
            printf("     MFT_ENUM_ADAPTER_LUID blob %u bytes, not a LUID\n", size);
        }
        return;
    }
    UINT64 packed = 0;
    if (type == MF_ATTRIBUTE_UINT64 && SUCCEEDED(attrs->GetUINT64(MFT_ENUM_ADAPTER_LUID, &packed))) {
        printf("     MFT_ENUM_ADAPTER_LUID uint64 0x%016llx\n",
               static_cast<unsigned long long>(packed));
        return;
    }
    printf("     MFT_ENUM_ADAPTER_LUID type %u\n", static_cast<unsigned>(type));
}

void PrintActivates(IMFActivate** activates, UINT32 count)
{
    for (UINT32 i = 0; i < count; ++i) {
        wchar_t* name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len))) {
            printf("  %u  %ls\n", i, name);
            CoTaskMemFree(name);
        } else {
            printf("  %u  (no friendly name)\n", i);
        }
        GUID clsid = GUID_NULL;
        if (SUCCEEDED(activates[i]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
            printf("     clsid ");
            PrintGuid(clsid);
            printf("\n");
        }
        wchar_t* url = nullptr;
        if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &url, &len))) {
            printf("     hardware url %ls\n", url);
            CoTaskMemFree(url);
        }
        UINT32 v = 0;
        if (SUCCEEDED(activates[i]->GetUINT32(MF_TRANSFORM_ASYNC, &v))) {
            printf("     MF_TRANSFORM_ASYNC %u\n", v);
        }
        if (SUCCEEDED(activates[i]->GetUINT32(MF_SA_D3D11_AWARE, &v))) {
            printf("     MF_SA_D3D11_AWARE %u\n", v);
        }
        PrintAdapterLuid(activates[i]);
    }
}

void ReleaseActivates(IMFActivate** activates, UINT32 count)
{
    for (UINT32 i = 0; i < count; ++i) {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
}

// What an application sees when it looks for a hardware H.264 encoder, in the two shapes that
// matter for a driver package:
//
//   - the whole machine (MFTEnumEx), which is what route A of INSTALL.md affects;
//   - one video adapter at a time (MFTEnum2 with MFT_ENUM_ADAPTER_LUID), which is how a client that
//     wants the encoder of a particular GPU asks. That call needs MFT_ENUM_FLAG_HARDWARE, and
//     MFTEnum2 returns E_INVALIDARG without it; the refusal is printed here as a control, so the
//     per-adapter listing below cannot be an empty result of calling the function wrongly.
//
// Nothing here writes to the registry.
int DoEnum()
{
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };
    const UINT32 flags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                         MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SYNCMFT;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &activates,
                           &count);
    if (FAILED(hr)) {
        return Fail("MFTEnumEx", hr);
    }
    printf("machine wide: %u H.264 encoder MFT(s)\n", count);
    PrintActivates(activates, count);
    ReleaseActivates(activates, count);

    ComPtr<IDXGIFactory1> factory;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) {
        return Fail("CreateDXGIFactory1", hr);
    }

    ComPtr<IDXGIAdapter1> first;
    if (SUCCEEDED(factory->EnumAdapters1(0, &first))) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(first->GetDesc1(&desc))) {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(MFCreateAttributes(&attrs, 1)) &&
                SUCCEEDED(attrs->SetBlob(MFT_ENUM_ADAPTER_LUID,
                                         reinterpret_cast<const UINT8*>(&desc.AdapterLuid),
                                         sizeof(desc.AdapterLuid)))) {
                IMFActivate** none = nullptr;
                UINT32 noneCount = 0;
                const HRESULT refused =
                    MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER,
                             MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SYNCMFT, nullptr, &outInfo,
                             attrs.Get(), &none, &noneCount);
                printf("\ncontrol: an adapter LUID without MFT_ENUM_FLAG_HARDWARE answers 0x%08lX"
                       " (E_INVALIDARG is 0x%08lX)\n",
                       static_cast<unsigned long>(refused),
                       static_cast<unsigned long>(E_INVALIDARG));
                if (SUCCEEDED(refused)) {
                    ReleaseActivates(none, noneCount);
                }
            }
        }
    }

    for (UINT32 a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapters1(a, &adapter))) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        printf("\nadapter %u  %04x:%04x  luid %08lx:%08lx  %ls\n", a, desc.VendorId, desc.DeviceId,
               static_cast<unsigned long>(desc.AdapterLuid.HighPart),
               static_cast<unsigned long>(desc.AdapterLuid.LowPart), desc.Description);
        ComPtr<IMFAttributes> attrs;
        hr = MFCreateAttributes(&attrs, 1);
        if (SUCCEEDED(hr)) {
            hr = attrs->SetBlob(MFT_ENUM_ADAPTER_LUID,
                                reinterpret_cast<const UINT8*>(&desc.AdapterLuid),
                                sizeof(desc.AdapterLuid));
        }
        if (FAILED(hr)) {
            return Fail("MFCreateAttributes/SetBlob", hr);
        }
        activates = nullptr;
        count = 0;
        hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, attrs.Get(), &activates,
                      &count);
        if (FAILED(hr)) {
            printf("  MFTEnum2 0x%08lX\n", static_cast<unsigned long>(hr));
            continue;
        }
        printf("  %u H.264 encoder MFT(s) for this adapter\n", count);
        PrintActivates(activates, count);
        ReleaseActivates(activates, count);
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const wchar_t* mode = (argc > 1) ? argv[1] : L"--show";
    MfStartup mf;
    if (FAILED(mf.hr)) {
        return Fail("MFStartup", mf.hr);
    }

    if (wcscmp(mode, L"--show") == 0) {
        return DoShow();
    }
    if (wcscmp(mode, L"--inf") == 0) {
        return DoInf();
    }
    if (wcscmp(mode, L"--reg") == 0) {
        return DoReg();
    }
    if (wcscmp(mode, L"--enum") == 0) {
        return DoEnum();
    }
    if (wcscmp(mode, L"--register-global") == 0 || wcscmp(mode, L"--unregister-global") == 0) {
        bool consent = false;
        for (int i = 2; i < argc; ++i) {
            if (wcscmp(argv[i], L"--yes-lab") == 0) {
                consent = true;
            }
        }
        wchar_t env[8] = {};
        const DWORD n = GetEnvironmentVariableW(L"BC250_ALLOW_HKLM_MFT", env, ARRAYSIZE(env));
        const bool envOk = (n == 1 && env[0] == L'1');
        if (!consent || !envOk) {
            fprintf(stderr, "refused: machine-wide registration needs --yes-lab and "
                            "BC250_ALLOW_HKLM_MFT=1.\n"
                            "This writes under HKEY_LOCAL_MACHINE and is only ever run on the lab.\n");
            return 3;
        }
        wchar_t host[64] = {};
        DWORD hostLen = ARRAYSIZE(host);
        GetComputerNameW(host, &hostLen);
        fprintf(stderr, "machine-wide registration on %ls\n", host);
        const HRESULT hr = (wcscmp(mode, L"--register-global") == 0) ? RegisterGlobal(true)
                                                                    : UnregisterGlobal(true);
        if (FAILED(hr)) {
            return Fail("MFTRegister", hr);
        }
        printf("ok\n");
        return 0;
    }

    fprintf(stderr, "usage: mftreg.exe [--show|--inf|--reg|--enum|--register-global --yes-lab]\n");
    return 1;
}
