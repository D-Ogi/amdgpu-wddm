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
//   mftreg.exe --enum       what MFTEnum2 reports for hardware H.264 encoders on this machine
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

void PrintInfBinary(const char* valueName, const std::vector<uint8_t>& bytes)
{
    // FLG_ADDREG_BINVALUETYPE is 0x00000001; the value is a comma separated list of hex bytes.
    printf("HKR,,\"%s\",0x00000001,", valueName);
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
    printf("; AddReg section for the H.264 encoder MFT. Keys are relative to\n");
    printf("; HKLM\\SOFTWARE\\Classes\\MediaFoundation\\Transforms\\{clsid}, so the AddReg entry that\n");
    printf("; uses this section has to name that key with HKLM and the full path.\n");
    printf("HKR,,,,\"%ls\"\n", RegistrationName());
    uint32_t f = 0;
    memcpy(&f, flags.data(), sizeof(f));
    printf("HKR,,\"MFTFlags\",0x00010001,0x%08x\n", f);
    PrintInfBinary("InputTypes", inTypes);
    PrintInfBinary("OutputTypes", outTypes);
    PrintInfBinary("Attributes", attrs);
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
    wchar_t category[64] = {};
    StringFromGUID2(MFT_CATEGORY_VIDEO_ENCODER, category, ARRAYSIZE(category));
    uint32_t f = 0;
    memcpy(&f, flags.data(), sizeof(f));

    printf("Windows Registry Editor Version 5.00\n\n");
    printf("[HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\MediaFoundation\\Transforms\\%ls]\n", clsid);
    printf("@=\"%ls\"\n", RegistrationName());
    printf("\"MFTFlags\"=dword:%08x\n", f);
    PrintRegBinary("InputTypes", inTypes);
    PrintRegBinary("OutputTypes", outTypes);
    PrintRegBinary("Attributes", attrs);
    printf("\n[HKEY_LOCAL_MACHINE\\SOFTWARE\\Classes\\MediaFoundation\\Transforms\\Categories\\%ls\\%ls]\n",
           category, clsid);
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

int DoEnum()
{
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                           MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                               MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SYNCMFT,
                           nullptr, &outInfo, &activates, &count);
    if (FAILED(hr)) {
        return Fail("MFTEnumEx", hr);
    }
    printf("%u H.264 encoder MFT(s)\n", count);
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
        activates[i]->Release();
    }
    CoTaskMemFree(activates);
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
