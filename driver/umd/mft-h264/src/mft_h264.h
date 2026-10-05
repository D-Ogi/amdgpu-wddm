// SPDX-License-Identifier: MIT
// The Media Foundation hardware encoder transform.
//
// Contract, from learn.microsoft.com/windows/win32/medfound/hardware-mfts and the SDK headers in
// toolchain/nuget (10.0.26100.0):
//   - asynchronous model: MF_TRANSFORM_ASYNC = 1 (mftransform.h:1640), the client unlocks us with
//     MF_TRANSFORM_ASYNC_UNLOCK (mftransform.h:1641) and we refuse to work until it does;
//   - events METransformNeedInput / METransformHaveOutput / METransformDrainComplete through
//     IMFMediaEventGenerator, delegated to an IMFMediaEventQueue (mfapi.h:649);
//   - MFT_SUPPORT_DYNAMIC_FORMAT_CHANGE = 1 (mfapi.h:2144);
//   - MFT_ENUM_HARDWARE_URL_Attribute (mftransform.h:1647) so that the topology loader treats us as a
//     hardware transform; its value is documented as an opaque string that the loader does not parse;
//   - MFT_ENUM_HARDWARE_VENDOR_ID_Attribute (mftransform.h:1633), reference only;
//   - MF_SA_D3D11_AWARE = 1 (mftransform.h:1618) and MF_SA_D3D_AWARE (mftransform.h:694) so that a
//     client sends MFT_MESSAGE_SET_D3D_MANAGER with an IMFDXGIDeviceManager and hands us textures;
//   - IMFShutdown (mfidl.h:7420), ICodecAPI (icodecapi.h) for the encoder settings Game Bar and
//     Chromium write.
//
// Threading: the encode runs on the thread that calls ProcessInput, inside
// IMFDXGIDeviceManager::LockDevice, so it is serialised against the client's own use of the same
// Direct3D device. That is permitted - ProcessInput may block - and avoids a second device context.
// The output sample is complete before METransformHaveOutput is queued, so a client that calls
// ProcessOutput from the event callback finds consistent state.

#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <d3d11.h>
#include "encoder.h"

// {A32438F0-0D79-4CA9-A5BF-9F3C80837253}
extern "C" const GUID CLSID_Bc250H264EncoderMFT;

// Pictures encoded by every instance of this transform in the process since it was loaded. This is
// how a test that drives a Media Foundation pipeline establishes that the pipeline really used our
// transform and not another encoder: the pipeline picks the transform, we only count.
extern "C" long __stdcall Bc250H264EncodedPictureCount(void);

namespace bc250h264 {

// The module lock DllCanUnloadNow answers from (dllmain.cpp). Every object this DLL hands out holds
// one for its whole lifetime, because COM does not call IClassFactory::LockServer for an in-process
// server and the host may call CoFreeUnusedLibraries while our transform is still streaming.
void ModuleLock();
void ModuleUnlock();
// True when nothing this DLL handed out is alive any more. DllCanUnloadNow's whole answer.
bool ModuleIsIdle();

class Bc250H264Mft : public IMFTransform,
                     public IMFMediaEventGenerator,
                     public IMFShutdown,
                     public IMFAttributes,
                     public ICodecAPI,
                     public IMFRealTimeClientEx {
public:
    Bc250H264Mft();
    HRESULT Construct();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IMFTransform
    HRESULT STDMETHODCALLTYPE GetStreamLimits(DWORD*, DWORD*, DWORD*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE GetStreamCount(DWORD*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE GetStreamIDs(DWORD, DWORD*, DWORD, DWORD*) override;
    HRESULT STDMETHODCALLTYPE GetInputStreamInfo(DWORD, MFT_INPUT_STREAM_INFO*) override;
    HRESULT STDMETHODCALLTYPE GetOutputStreamInfo(DWORD, MFT_OUTPUT_STREAM_INFO*) override;
    HRESULT STDMETHODCALLTYPE GetAttributes(IMFAttributes**) override;
    HRESULT STDMETHODCALLTYPE GetInputStreamAttributes(DWORD, IMFAttributes**) override;
    HRESULT STDMETHODCALLTYPE GetOutputStreamAttributes(DWORD, IMFAttributes**) override;
    HRESULT STDMETHODCALLTYPE DeleteInputStream(DWORD) override;
    HRESULT STDMETHODCALLTYPE AddInputStreams(DWORD, DWORD*) override;
    HRESULT STDMETHODCALLTYPE GetInputAvailableType(DWORD, DWORD, IMFMediaType**) override;
    HRESULT STDMETHODCALLTYPE GetOutputAvailableType(DWORD, DWORD, IMFMediaType**) override;
    HRESULT STDMETHODCALLTYPE SetInputType(DWORD, IMFMediaType*, DWORD) override;
    HRESULT STDMETHODCALLTYPE SetOutputType(DWORD, IMFMediaType*, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetInputCurrentType(DWORD, IMFMediaType**) override;
    HRESULT STDMETHODCALLTYPE GetOutputCurrentType(DWORD, IMFMediaType**) override;
    HRESULT STDMETHODCALLTYPE GetInputStatus(DWORD, DWORD*) override;
    HRESULT STDMETHODCALLTYPE GetOutputStatus(DWORD*) override;
    HRESULT STDMETHODCALLTYPE SetOutputBounds(LONGLONG, LONGLONG) override;
    HRESULT STDMETHODCALLTYPE ProcessEvent(DWORD, IMFMediaEvent*) override;
    HRESULT STDMETHODCALLTYPE ProcessMessage(MFT_MESSAGE_TYPE, ULONG_PTR) override;
    HRESULT STDMETHODCALLTYPE ProcessInput(DWORD, IMFSample*, DWORD) override;
    HRESULT STDMETHODCALLTYPE ProcessOutput(DWORD, DWORD, MFT_OUTPUT_DATA_BUFFER*, DWORD*) override;

    // IMFMediaEventGenerator
    HRESULT STDMETHODCALLTYPE GetEvent(DWORD, IMFMediaEvent**) override;
    HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback*, IUnknown*) override;
    HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult*, IMFMediaEvent**) override;
    HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType, REFGUID, HRESULT,
                                         const PROPVARIANT*) override;

    // IMFRealTimeClientEx. The frame server and the Media Foundation capture engine query this on
    // every transform in a real time topology and hand out the multithreaded work queue the whole
    // topology shares. We do all our work on the caller's thread, so there is no thread of ours to
    // register; the queue id is remembered so that any future work item of ours lands in it rather
    // than on the platform's default queue.
    HRESULT STDMETHODCALLTYPE RegisterThreadsEx(DWORD*, LPCWSTR, LONG) override;
    HRESULT STDMETHODCALLTYPE UnregisterThreads() override;
    HRESULT STDMETHODCALLTYPE SetWorkQueueEx(DWORD, LONG) override;

    // IMFShutdown
    HRESULT STDMETHODCALLTYPE Shutdown() override;
    HRESULT STDMETHODCALLTYPE GetShutdownStatus(MFSHUTDOWN_STATUS*) override;

    // IMFAttributes, delegated to the transform's own attribute store
    HRESULT STDMETHODCALLTYPE GetItem(REFGUID, PROPVARIANT*) override;
    HRESULT STDMETHODCALLTYPE GetItemType(REFGUID, MF_ATTRIBUTE_TYPE*) override;
    HRESULT STDMETHODCALLTYPE CompareItem(REFGUID, REFPROPVARIANT, BOOL*) override;
    HRESULT STDMETHODCALLTYPE Compare(IMFAttributes*, MF_ATTRIBUTES_MATCH_TYPE, BOOL*) override;
    HRESULT STDMETHODCALLTYPE GetUINT32(REFGUID, UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetUINT64(REFGUID, UINT64*) override;
    HRESULT STDMETHODCALLTYPE GetDouble(REFGUID, double*) override;
    HRESULT STDMETHODCALLTYPE GetGUID(REFGUID, GUID*) override;
    HRESULT STDMETHODCALLTYPE GetStringLength(REFGUID, UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetString(REFGUID, LPWSTR, UINT32, UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetAllocatedString(REFGUID, LPWSTR*, UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetBlobSize(REFGUID, UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetBlob(REFGUID, UINT8*, UINT32, UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetAllocatedBlob(REFGUID, UINT8**, UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetUnknown(REFGUID, REFIID, LPVOID*) override;
    HRESULT STDMETHODCALLTYPE SetItem(REFGUID, REFPROPVARIANT) override;
    HRESULT STDMETHODCALLTYPE DeleteItem(REFGUID) override;
    HRESULT STDMETHODCALLTYPE DeleteAllItems() override;
    HRESULT STDMETHODCALLTYPE SetUINT32(REFGUID, UINT32) override;
    HRESULT STDMETHODCALLTYPE SetUINT64(REFGUID, UINT64) override;
    HRESULT STDMETHODCALLTYPE SetDouble(REFGUID, double) override;
    HRESULT STDMETHODCALLTYPE SetGUID(REFGUID, REFGUID) override;
    HRESULT STDMETHODCALLTYPE SetString(REFGUID, LPCWSTR) override;
    HRESULT STDMETHODCALLTYPE SetBlob(REFGUID, const UINT8*, UINT32) override;
    HRESULT STDMETHODCALLTYPE SetUnknown(REFGUID, IUnknown*) override;
    HRESULT STDMETHODCALLTYPE LockStore() override;
    HRESULT STDMETHODCALLTYPE UnlockStore() override;
    HRESULT STDMETHODCALLTYPE GetCount(UINT32*) override;
    HRESULT STDMETHODCALLTYPE GetItemByIndex(UINT32, GUID*, PROPVARIANT*) override;
    HRESULT STDMETHODCALLTYPE CopyAllItems(IMFAttributes*) override;

    // ICodecAPI
    HRESULT STDMETHODCALLTYPE IsSupported(const GUID*) override;
    HRESULT STDMETHODCALLTYPE IsModifiable(const GUID*) override;
    HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID*, VARIANT*, VARIANT*, VARIANT*) override;
    HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID*, VARIANT**, ULONG*) override;
    HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID*, VARIANT*) override;
    HRESULT STDMETHODCALLTYPE GetValue(const GUID*, VARIANT*) override;
    HRESULT STDMETHODCALLTYPE SetValue(const GUID*, VARIANT*) override;
    HRESULT STDMETHODCALLTYPE RegisterForEvent(const GUID*, LONG_PTR) override;
    HRESULT STDMETHODCALLTYPE UnregisterForEvent(const GUID*) override;
    HRESULT STDMETHODCALLTYPE SetAllDefaults() override;
    HRESULT STDMETHODCALLTYPE SetValueWithNotify(const GUID*, VARIANT*, GUID**, ULONG*) override;
    HRESULT STDMETHODCALLTYPE SetAllDefaultsWithNotify(GUID**, ULONG*) override;
    HRESULT STDMETHODCALLTYPE GetAllSettings(IStream*) override;
    HRESULT STDMETHODCALLTYPE SetAllSettings(IStream*) override;
    HRESULT STDMETHODCALLTYPE SetAllSettingsWithNotify(IStream*, GUID**, ULONG*) override;

private:
    ~Bc250H264Mft();
    HRESULT CheckValid() const;
    HRESULT EnsureEncoder();
    // Rebuilds MF_MT_MPEG_SEQUENCE_HEADER and MF_MT_MPEG2_LEVEL on the current output type from
    // m_cfg. Called from every path that changes something the parameter sets carry.
    HRESULT RefreshOutputParameterSets();
    // Publishes MFT_ENUM_ADAPTER_LUID of the adapter the encode runs on, once it is known.
    void PublishAdapterLuid(ID3D11Device* device);
    HRESULT BuildInputType(DWORD index, IMFMediaType** out) const;
    HRESULT BuildOutputType(IMFMediaType** out) const;
    // Holds whatever has to stay alive and locked while the encode reads the input sample.
    struct FrameLock {
        ComPtr<IMFMediaBuffer> buffer;
        ComPtr<IMF2DBuffer2> buffer2d;
        ComPtr<ID3D11Texture2D> texture;
        bool locked2d = false;
        bool locked1d = false;
        ~FrameLock() { Release(); }
        void Release();
    };
    HRESULT SampleToFrame(IMFSample* sample, GpuFrameInput* frame, FrameLock* lock);
    HRESULT QueueNeedInput();

    LONG m_refCount = 1;
    mutable CRITICAL_SECTION m_lock = {};
    bool m_lockInit = false;
    bool m_shutdown = false;
    bool m_streaming = false;
    bool m_haveOutput = false;
    // True while a METransformNeedInput we queued has not been answered by a ProcessInput. The
    // asynchronous contract gives one credit per free input slot and we hold exactly one slot, so
    // this stops a second credit being issued; a drain or a flush makes the client drop whatever
    // request it still holds, so it is cleared there and the next start of stream re-issues it.
    bool m_inputRequested = false;
    // The work queue and base priority the real time client contract handed us,
    // MFASYNC_CALLBACK_QUEUE_UNDEFINED while none was set. Recorded, not used: every stage of the
    // encode runs on the thread that calls ProcessInput, so this transform queues no work item of
    // its own. The first one that needs one has to run in this queue, at this priority, or it
    // escapes the topology's own scheduling.
    DWORD m_workQueue = MFASYNC_CALLBACK_QUEUE_UNDEFINED;
    LONG m_workItemPriority = 0;

    ComPtr<IMFAttributes> m_attributes;
    ComPtr<IMFMediaEventQueue> m_events;
    ComPtr<IMFMediaType> m_inputType;
    ComPtr<IMFMediaType> m_outputType;
    ComPtr<IMFDXGIDeviceManager> m_deviceManager;
    ComPtr<IMFSample> m_pendingOutput;

    GUID m_inputSubtype = GUID_NULL;
    EncoderConfig m_cfg;
    Encoder m_encoder;
    bool m_encoderReady = false;
    bool m_forceKeyFrame = false;
    std::vector<uint8_t> m_bitstream;
    std::vector<uint8_t> m_repack;   // only used for bottom-up (negative pitch) system memory input
    LONGLONG m_lastDuration = 0;
};

} // namespace bc250h264
