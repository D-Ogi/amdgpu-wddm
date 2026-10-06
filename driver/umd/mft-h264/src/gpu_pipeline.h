// SPDX-License-Identifier: MIT
// Direct3D 11 host side of the encoder's GPU half.
//
// Everything runs as compute on the device the client gave us through IMFDXGIDeviceManager, which on
// unit A is our own driver's device, so the dispatches below execute on the BC-250 GPU. When no device
// manager is set the encoder creates its own device on the BC-250 adapter (1002:13FE) and on no other
// adapter: it answers DXGI_ERROR_NOT_FOUND when that adapter is absent, so a client that asked Media
// Foundation for a hardware encoder goes back to the encoder of Windows instead of to a software
// encode on this transform.

#pragma once
#include <d3d11.h>
#include <stdint.h>
#include <vector>
#include "mb_layout.h"

namespace bc250h264 {

template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : m_p(o.m_p) { o.m_p = nullptr; }
    // Note the comparison on the raw pointers: operator& below is the "pass me to a COM creation
    // function" overload, so `this != &o` would not compile here.
    ComPtr& operator=(ComPtr&& o) noexcept
    {
        if (m_p != o.m_p) { Reset(); m_p = o.m_p; o.m_p = nullptr; }
        return *this;
    }
    T** operator&() { Reset(); return &m_p; }
    T* Get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    void Reset() { if (m_p) { m_p->Release(); m_p = nullptr; } }
    void Attach(T* p) { Reset(); m_p = p; }
    T* Detach() { T* p = m_p; m_p = nullptr; return p; }
    void CopyFrom(T* p) { Reset(); m_p = p; if (m_p) { m_p->AddRef(); } }

private:
    T* m_p = nullptr;
};

// Diagnostic trace of one encode, off unless BC250_MFT_TRACE is set in the environment. It names
// the calling thread, so the order of ProcessInput, ProcessOutput, the queued events and the GPU
// stage is readable across the client's worker threads. It is written straight to the standard error
// handle: no CRT stream state is shared with a host we are loaded into. It lives at this level
// because every layer above it traces through it.
int MftTraceOn();
void MftTrace(const char* fmt, ...);

// The frame sizes this encoder can code: 16 to 4096 samples each way, and even in both directions.
// 4:2:0 chroma is sampled on an even grid and the SPS crops in units of two luma samples
// (CropUnitX == CropUnitY == 2 for frame_mbs_only_flag 1), so an odd visible size has no
// representation in the bitstream. Every entry point refuses one instead of rounding it down: the
// transform in SetInputType and SetOutputType, where the client can still renegotiate, and
// GpuEncoder::Initialize for a direct caller.
bool IsCodableFrameSize(uint32_t width, uint32_t height);

enum class InputKind { Planar8, Nv12Sys, BgraSys, TextureNV12, TextureBGRA };

struct GpuFrameInput {
    InputKind kind = InputKind::Planar8;
    // Planar8: three planes of the visible size, Y then Cb then Cr.
    // Nv12Sys: planeY is the luma plane, planeCb the interleaved CbCr plane (pitchC in bytes).
    const uint8_t* planeY = nullptr;
    const uint8_t* planeCb = nullptr;
    const uint8_t* planeCr = nullptr;
    uint32_t pitchY = 0;
    uint32_t pitchC = 0;
    // BgraSys: packed BGRA8 of the visible size, pitchRgb bytes per row.
    const uint8_t* rgb = nullptr;
    uint32_t pitchRgb = 0;
    // Texture*: a Direct3D 11 texture on the same device.
    ID3D11Texture2D* texture = nullptr;
    // Which array slice of that texture holds the picture. Game Bar, the Windows frame server and
    // Windows.Graphics.Capture all hand out slices of a texture array, so this is the normal case and
    // not an exception; slices other than 0 cost one GPU copy per picture (see ShaderReadableTexture).
    uint32_t slice = 0;
};

struct GpuFrameParams {
    bool intra = true;
    uint32_t qpY = 26;
    uint32_t qpC = 26;
    uint32_t lambda = 2;
    uint32_t skipBias = 0;
    uint32_t deblockIdc = 1;
    int32_t alphaOffsetDiv2 = 0;
    int32_t betaOffsetDiv2 = 0;
};

// The stages of one picture's GPU work, in the order the command stream holds them. GpuStageMode is
// the macroblock pass: on an I picture the anti-diagonal sweep of cs_mb's intra entry point, one
// dispatch per anti-diagonal, and on a P picture one dispatch of its inter entry point.
enum : uint32_t {
    GpuStageImport = 0,
    GpuStageMotion = 1,
    GpuStageMode = 2,
    GpuStageDeblock = 3,
    GpuStageCount = 4
};
const char* GpuStageName(uint32_t stage);

// One dispatch of the picture with the GPU time between the timestamp before it and the one after
// it. Filled only at BC250_MFT_STAGE_TIMING=2.
struct GpuStageStep {
    uint32_t stage = 0;
    uint32_t groups = 0;   // thread groups, which is macroblocks for cs_mb and cs_deblock
    double ms = 0.0;
};

// Where one picture's GPU time went. Off unless BC250_MFT_STAGE_TIMING is set in the environment:
// "1" times the four stages, "2" also times every dispatch. Every mark is one more timestamp query
// inside the command stream, and a 1080p I picture holds about 440 dispatches, so level 2 reports a
// total above the same picture's uninstrumented cost. It says where the time goes; it does not quote
// a throughput.
struct GpuStageProfile {
    bool valid = false;
    uint32_t level = 0;
    uint32_t dispatches[GpuStageCount] = {};
    uint32_t groups[GpuStageCount] = {};
    double ms[GpuStageCount] = {};
    double totalMs = 0.0;        // the first mark to the last one
    uint32_t marksDropped = 0;   // dispatches past the mark budget, which are not timed
    std::vector<GpuStageStep> steps;
};

class GpuEncoder {
public:
    // device may be null, in which case a device is created on the preferred adapter.
    HRESULT Initialize(ID3D11Device* device, uint32_t visibleWidth, uint32_t visibleHeight);
    void Shutdown();

    uint32_t WidthMb() const { return m_widthMb; }
    uint32_t HeightMb() const { return m_heightMb; }
    uint32_t PadW() const { return m_padW; }
    uint32_t PadH() const { return m_padH; }
    uint32_t MbCount() const { return m_widthMb * m_heightMb; }
    ID3D11Device* Device() const { return m_device.Get(); }

    // Runs one picture through the GPU and reads back the levels and the per-macroblock info.
    HRESULT EncodeFrame(const GpuFrameInput& in, const GpuFrameParams& p,
                        std::vector<uint32_t>& levels, std::vector<MbInfo>& info);

    // The reconstruction of the picture just encoded, in coded (padded) dimensions. Test and
    // diagnostic use only; the encoder itself never moves the reconstruction to the CPU.
    HRESULT ReadReconstruction(std::vector<uint8_t>& y, std::vector<uint8_t>& cb,
                               std::vector<uint8_t>& cr);
    // The source picture as the import pass produced it, in coded dimensions. Used for PSNR.
    HRESULT ReadSource(std::vector<uint8_t>& y, std::vector<uint8_t>& cb, std::vector<uint8_t>& cr);

    // Marks the reconstruction just produced as the reference for the next picture.
    void SwapReference();

    // The GPU time between the first and the last dispatch of the last picture, from the device's own
    // timestamps. Zero, and LastGpuTimingValid false, when the device did not deliver a usable pair:
    // never the previous picture's number (see the query wait in EncodeFrame).
    double LastGpuMilliseconds() const { return m_lastGpuMs; }
    bool LastGpuTimingValid() const { return m_lastGpuMsValid; }
    // Wall-clock cost of moving the last picture's levels and macroblock info to the CPU: the two
    // CopyResource calls, the two blocking Map calls and the two memcpy calls. This is where the CPU
    // waits for the GPU, so it is the one number that says whether the pipeline is GPU bound.
    double LastReadbackMilliseconds() const { return m_lastReadbackMs; }
    // How long the last picture's timestamp queries took to retire after the readback. Part of the
    // GPU stage, and the price of a per-picture GPU measurement.
    double LastQueryWaitMilliseconds() const { return m_lastQueryWaitMs; }
    // Wall clock the calling thread spent recording the last picture's commands: the input upload,
    // every constant buffer Map and every Dispatch call, up to the closing timestamp. A 1080p I
    // picture records 443 dispatches, so this is a cost of its own, and in the present
    // one-picture-at-a-time shape it is serial with the GPU rather than overlapped with it.
    double LastRecordMilliseconds() const { return m_lastRecordMs; }
    // The blocking Map on the levels staging buffer, which is where the CPU waits for the whole
    // picture to finish. The rest of the readback stage is the two copies and the two memcpy calls,
    // so LastReadbackMilliseconds() minus this is the transfer, and this is the wait.
    double LastMapWaitMilliseconds() const { return m_lastMapWaitMs; }
    // Where the last picture's GPU time went, stage by stage; see GpuStageProfile. Valid only with
    // BC250_MFT_STAGE_TIMING set in the environment when Initialize ran.
    const GpuStageProfile& LastStageProfile() const { return m_profile; }

private:
    struct Plane {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11UnorderedAccessView> uav;
        uint32_t bytes = 0;
    };

    HRESULT CreateRawBuffer(uint32_t bytes, bool uav, Plane* out);
    HRESULT CreateUploadBuffer(uint32_t bytes, Plane* out);
    HRESULT CreateStaging(uint32_t bytes, ID3D11Buffer** out);
    HRESULT CompileShaders();
    void UpdateConstants(const GpuFrameParams& p, uint32_t diagonal, uint32_t diagonalBase);
    HRESULT UploadPlanar(const GpuFrameInput& in);
    // Returns a texture the compute pass can read: the client's own if it is a shader bindable,
    // single slice texture, else a private copy of the same format holding the wanted array slice.
    HRESULT ShaderReadableTexture(ID3D11Texture2D* src, bool nv12, uint32_t slice,
                                  ID3D11Texture2D** out);
    HRESULT UploadBgraSystem(const uint8_t* rgb, uint32_t pitch, ID3D11Texture2D** out);
    HRESULT ReadPlanes(Plane* y, Plane* cb, Plane* cr, std::vector<uint8_t>& oy,
                       std::vector<uint8_t>& ocb, std::vector<uint8_t>& ocr);
    // Reads one query to completion. ID3D11DeviceContext::GetData answers S_FALSE while the query
    // has not retired, so a single call can leave the caller with nothing; this waits, with a bound.
    HRESULT WaitForQuery(ID3D11Asynchronous* query, void* data, uint32_t bytes);

    // The stage profile, all four no-ops at BC250_MFT_STAGE_TIMING unset. BeginStageTiming starts a
    // picture, CountDispatch follows every Dispatch, MarkStageEnd closes a stage at level 1 (at
    // level 2 the last dispatch's own mark already closes it), and CollectStageTiming reads the
    // timestamps once the picture has retired.
    void BeginStageTiming();
    void CountDispatch(uint32_t stage, uint32_t groups);
    void MarkStageEnd(uint32_t stage);
    void CollectStageTiming(const D3D11_QUERY_DATA_TIMESTAMP_DISJOINT& dj);
    bool PlaceMark(uint32_t stage, uint32_t groups);

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11ComputeShader> m_csImportNV12;
    ComPtr<ID3D11ComputeShader> m_csImportNV12Sys;
    ComPtr<ID3D11ComputeShader> m_csImportBGRA;
    ComPtr<ID3D11ComputeShader> m_csImportPlanar;
    ComPtr<ID3D11ComputeShader> m_csMotion;
    ComPtr<ID3D11ComputeShader> m_csEncodeIntra;
    ComPtr<ID3D11ComputeShader> m_csEncodeInter;
    ComPtr<ID3D11ComputeShader> m_csDeblock;
    ComPtr<ID3D11Buffer> m_cb;

    Plane m_src[3];
    Plane m_rec[2][3];        // ping-pong: m_rec[m_cur] is being written, m_rec[1-m_cur] is reference
    Plane m_upload[3];
    Plane m_levels;
    Plane m_mbinfo;
    ComPtr<ID3D11Buffer> m_levelsStaging;
    ComPtr<ID3D11Buffer> m_mbinfoStaging;
    ComPtr<ID3D11Buffer> m_planeStaging;
    ComPtr<ID3D11Texture2D> m_ownNv12;
    ComPtr<ID3D11Texture2D> m_ownBgra;
    ComPtr<ID3D11Query> m_tsDisjoint;
    ComPtr<ID3D11Query> m_tsBegin;
    ComPtr<ID3D11Query> m_tsEnd;
    // The stage marks: one timestamp query per mark, grown once and reused for every picture after.
    std::vector<ComPtr<ID3D11Query>> m_marks;
    std::vector<uint32_t> m_markStage;    // which stage the dispatch before mark i belongs to
    std::vector<uint32_t> m_markGroups;   // its thread groups
    uint32_t m_markCount = 0;
    uint32_t m_stageLevel = 0;
    GpuStageProfile m_profile;

    uint32_t m_visW = 0, m_visH = 0;
    uint32_t m_padW = 0, m_padH = 0;
    uint32_t m_widthMb = 0, m_heightMb = 0;
    uint32_t m_cur = 0;
    uint32_t m_lastWritten = 0;   // the m_rec index EncodeFrame wrote last, for ReadReconstruction
    double m_lastGpuMs = 0.0;
    double m_lastReadbackMs = 0.0;
    double m_lastQueryWaitMs = 0.0;
    double m_lastRecordMs = 0.0;
    double m_lastMapWaitMs = 0.0;
    bool m_lastGpuMsValid = false;
};

} // namespace bc250h264
