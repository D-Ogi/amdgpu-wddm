// Registration data for the encoder transform.
//
// Three consumers, one source of truth:
//   - the driver package, which needs the four registry values a Media Foundation transform key
//     holds (see lab/INF-FRAGMENT.txt);
//   - the lab, which may register the transform for the whole machine with MFTRegister;
//   - the host test, which registers it only inside its own process with MFTRegisterLocal and needs
//     the very same type lists, so that a difference between test and shipped registration cannot
//     hide here.

#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <stdint.h>
#include <vector>

namespace bc250h264 {

// Which registry value a caller wants built.
enum class RegBlob : uint32_t {
    Attributes = 0,   // REG_BINARY, MFGetAttributesAsBlob of the enumeration attribute store
    InputTypes = 1,   // REG_BINARY, packed MFT_REGISTER_TYPE_INFO
    OutputTypes = 2,  // REG_BINARY, packed MFT_REGISTER_TYPE_INFO
    MftFlags = 3,     // REG_DWORD, the MFT_ENUM_FLAG combination
};

const MFT_REGISTER_TYPE_INFO* RegistrationInputTypes(uint32_t* count);
const MFT_REGISTER_TYPE_INFO* RegistrationOutputTypes(uint32_t* count);
const wchar_t* RegistrationName();
uint32_t RegistrationFlags();

// The attribute store that goes into the registry (and into MFTRegister). It is the enumeration
// view of the transform: what MFTEnum2 reports before anything is instantiated. It must agree with
// what Bc250H264Mft::Construct sets on the live object, or a client's enumeration-time decision and
// its run-time experience will differ.
HRESULT BuildRegistrationAttributes(IMFAttributes** out);

HRESULT BuildRegistrationBlob(RegBlob which, std::vector<uint8_t>& out);

// Registers the transform inside this process only: no registry, no CLSID, nothing left behind when
// the process exits. This is the route the host test uses.
HRESULT RegisterLocal(IClassFactory* factory);
HRESULT UnregisterLocal();

// Machine-wide registration under HKEY_LOCAL_MACHINE. Only ever called on the lab; the development
// PC must stay untouched, so every caller has to pass confirm = true explicitly.
HRESULT RegisterGlobal(bool confirm);
HRESULT UnregisterGlobal(bool confirm);

} // namespace bc250h264

// Exported so that a packaging step can read the exact bytes out of the shipped binary instead of a
// second, drifting copy in a script.
extern "C" HRESULT __stdcall Bc250BuildMftRegistration(uint32_t which, uint8_t* buffer,
                                                       uint32_t capacity, uint32_t* needed);
