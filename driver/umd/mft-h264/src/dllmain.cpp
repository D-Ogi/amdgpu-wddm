// SPDX-License-Identifier: MIT
// COM entry points for the encoder transform.
//
// There is deliberately no DllRegisterServer: a display driver package registers its MFT from the
// INF (see INSTALL.md), and a self-registering DLL in a driver package would fail driver package
// validation. Test processes instantiate the class directly or through MFTRegisterLocalByCLSID,
// which needs no registry at all.

#include "mft_h264.h"
#include <new>

namespace {

class ClassFactory : public IClassFactory {
public:
    ClassFactory() { bc250h264::ModuleLock(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const long n = InterlockedDecrement(&m_refCount);
        if (n == 0) {
            delete this;
        }
        return static_cast<ULONG>(n);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        bc250h264::Bc250H264Mft* mft = new (std::nothrow) bc250h264::Bc250H264Mft();
        if (mft == nullptr) {
            return E_OUTOFMEMORY;
        }
        HRESULT hr = mft->Construct();
        if (SUCCEEDED(hr)) {
            hr = mft->QueryInterface(riid, ppv);
        }
        mft->Release();
        return hr;
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
    {
        if (lock) {
            bc250h264::ModuleLock();
        } else {
            bc250h264::ModuleUnlock();
        }
        return S_OK;
    }

private:
    ~ClassFactory() { bc250h264::ModuleUnlock(); }

    long m_refCount = 1;
};

} // namespace

// Direct construction, for a test process that wants the transform without any registration at all.
extern "C" HRESULT __stdcall Bc250CreateH264EncoderMFT(REFIID riid, void** ppv)
{
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;
    bc250h264::Bc250H264Mft* mft = new (std::nothrow) bc250h264::Bc250H264Mft();
    if (mft == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = mft->Construct();
    if (SUCCEEDED(hr)) {
        hr = mft->QueryInterface(riid, ppv);
    }
    mft->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv)
{
    if (ppv == nullptr) {
        return E_POINTER;
    }
    *ppv = nullptr;
    if (clsid != CLSID_Bc250H264EncoderMFT) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    ClassFactory* factory = new (std::nothrow) ClassFactory();
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
    // S_OK means "nothing of mine is alive, unmapping me is safe". Every transform object and every
    // class object holds a module lock, so this cannot answer S_OK under a live IMFTransform.
    return bc250h264::ModuleIsIdle() ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
