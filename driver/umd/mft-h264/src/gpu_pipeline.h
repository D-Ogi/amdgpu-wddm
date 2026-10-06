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

// How the deblocking wavefront is driven. Rows is the default: one dispatch of heightMb thread
// groups, each walking its macroblock row and waiting on the row above through the rwProgress
// counters (cs_deblock.hlsl, CSDeblockRows). Waves is the earlier shape, one dispatch per value of
// t = mbx + 2 * mby, which needs no assumption about thread group residency. Serial is one macroblock
// per dispatch in raster order, which is clause 8.7 read literally and tells a schedule defect from a
// filter defect. BC250_MFT_DEBLOCK=wavefront and BC250_MFT_SERIAL_DEBLOCK select the other two.
enum class DeblockMode : uint32_t { Rows = 0, Waves = 1, Serial = 2 };

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

// The most pictures that may be in the GPU's hands at once. Two is enough to cover the entropy coding
// of one picture with the GPU work of the next, which is the whole of the serialisation this removes;
// a deeper pipeline only adds latency, and every slot costs one levels and one macroblock-info staging
// buffer (3.4 MB together at 1080p).
enum : uint32_t { kMaxPipelineDepth = 4 };
// What the transform runs unless a client asked for low latency or BC250_MFT_DEPTH says otherwise. The
// test reads it too, so that the pipelined speed row of --compare is the speed of the shipped shape and
// not of a number the test chose for itself.
enum : uint32_t { kShippedPipelineDepth = 2 };

class GpuEncoder {
public:
    // device may be null, in which case a device is created on the preferred adapter.
    HRESULT Initialize(ID3D11Device* device, uint32_t visibleWidth, uint32_t visibleHeight);
    void Shutdown();

    // How many pictures may be submitted before one is collected. One is the serial shape: Submit
    // records a picture's commands and Collect immediately waits for it, so the GPU is idle while the
    // CPU codes the slice and the CPU is idle while the GPU works. The pipeline is correct at any
    // depth because the device executes the command stream in order: the reconstruction a P picture
    // reads is written by the dispatches before it, and the copy that captures a picture's levels is
    // recorded before the next picture's dispatches overwrite them, so only the staging buffers and
    // the timestamp queries need one copy per slot.
    //
    // Must be called with nothing in flight, which in practice means right after Initialize. It
    // refuses nothing: a depth above kMaxPipelineDepth is clamped, and a depth above 1 is clamped to 1
    // while BC250_MFT_STAGE_TIMING is on, because the per-dispatch marks of two pictures would share
    // one query array and the profile would mix them.
    HRESULT SetPipelineDepth(uint32_t depth);
    uint32_t PipelineDepth() const { return m_depth; }
    uint32_t InFlight() const { return m_inFlight; }

    uint32_t WidthMb() const { return m_widthMb; }
    uint32_t HeightMb() const { return m_heightMb; }
    uint32_t PadW() const { return m_padW; }
    uint32_t PadH() const { return m_padH; }
    uint32_t MbCount() const { return m_widthMb * m_heightMb; }
    ID3D11Device* Device() const { return m_device.Get(); }

    // Records one picture's GPU work and the copy that captures its result, and returns without
    // waiting for any of it. The input is read, uploaded or referenced before this returns, so the
    // caller may release a system memory picture afterwards; a texture the client still owns is read
    // by the import dispatch later, which is why the transform holds the input sample until the
    // picture is collected (MFT_INPUT_STREAM_HOLDS_BUFFERS).
    //
    // Refuses with E_NOT_VALID_STATE when PipelineDepth() pictures are already in flight.
    HRESULT Submit(const GpuFrameInput& in, const GpuFrameParams& p);

    // Waits for the oldest submitted picture and moves its levels and per-macroblock info to the
    // caller. Refuses with E_NOT_VALID_STATE when nothing is in flight. The Last*Milliseconds figures
    // and the stage profile describe the picture this collected, and ReadReconstruction returns its
    // reconstruction.
    HRESULT Collect(std::vector<uint32_t>& levels, std::vector<MbInfo>& info);

    // Submit followed by Collect: one picture through the GPU, with the thread waiting for it. The
    // serial shape, and what every caller that does not pipeline uses.
    HRESULT EncodeFrame(const GpuFrameInput& in, const GpuFrameParams& p,
                        std::vector<uint32_t>& levels, std::vector<MbInfo>& info);

    // The reconstruction of the picture just collected, in coded (padded) dimensions. Test and
    // diagnostic use only; the encoder itself never moves the reconstruction to the CPU. At a pipeline
    // depth of 2 it still returns the collected picture's reconstruction: picture k wrote one of the
    // two reconstruction buffers and picture k+2 is the next to overwrite it, and k is collected
    // before k+2 is submitted. Above 2 that no longer holds - picture k+2 is submitted while k is
    // still uncollected and overwrites the buffer this would read - so it is refused with
    // E_NOT_VALID_STATE rather than answering with another picture's samples. Reading it does wait for
    // every picture submitted so far, so it costs the pipeline.
    HRESULT ReadReconstruction(std::vector<uint8_t>& y, std::vector<uint8_t>& cb,
                               std::vector<uint8_t>& cr);
    // The source picture as the import pass produced it, in coded dimensions. Used for PSNR. The
    // import writes one set of source planes, so this is the most recently submitted picture, not the
    // most recently collected one: at a depth above one it is only meaningful right after a Submit.
    HRESULT ReadSource(std::vector<uint8_t>& y, std::vector<uint8_t>& cb, std::vector<uint8_t>& cr);

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
    // Wall clock the calling thread spent recording the collected picture's commands: the input
    // upload, every constant buffer Map, every Dispatch call and the two copies that capture the
    // result. A cost of its own, and at a pipeline depth above one it is the part of a picture that
    // overlaps the GPU work of the picture before it.
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

    // One picture in flight. Everything a picture needs after its commands are recorded: the two
    // staging buffers its result was copied into, its own timestamp queries, and what Submit already
    // knows about it. The shaders, the reconstruction buffers, the levels and the macroblock info are
    // shared, because the device executes in order and each picture's copy into these buffers is
    // recorded before the next picture's dispatches.
    struct Slot {
        ComPtr<ID3D11Buffer> levelsStaging;
        ComPtr<ID3D11Buffer> mbinfoStaging;
        ComPtr<ID3D11Query> tsDisjoint;
        ComPtr<ID3D11Query> tsBegin;
        ComPtr<ID3D11Query> tsEnd;
        // The private texture an input that cannot be read in place is copied into, created on first
        // use and one per slot. One shared texture would undo the pipeline for exactly the input
        // shapes that need the copy - an array slice, a multisampled or non-shader-bindable texture,
        // BGRA in system memory - because the next picture's copy into it cannot start until this
        // picture's import dispatch has finished reading it, and D3D11's own hazard tracking would
        // enforce that wait. One per slot costs 8 MB of BGRA or 3 MB of NV12 at 1080p per slot.
        ComPtr<ID3D11Texture2D> ownNv12;
        ComPtr<ID3D11Texture2D> ownBgra;
        double recordMs = 0.0;
        uint32_t written = 0;   // the m_rec index this picture's reconstruction went to
    };

    HRESULT CreateSlot(Slot* out);

    HRESULT CreateRawBuffer(uint32_t bytes, bool uav, Plane* out);
    HRESULT CreateUploadBuffer(uint32_t bytes, Plane* out);
    HRESULT CreateStaging(uint32_t bytes, ID3D11Buffer** out);
    HRESULT CompileShaders();
    void UpdateConstants(const GpuFrameParams& p, uint32_t diagonal, uint32_t diagonalBase);
    HRESULT UploadPlanar(const GpuFrameInput& in);
    // Returns a texture the compute pass can read: the client's own if it is a shader bindable,
    // single slice texture, else a private copy of the same format holding the wanted array slice.
    // The copy goes into the submitting slot's own texture, so that one picture's copy never waits
    // for the picture before it to stop reading.
    HRESULT ShaderReadableTexture(Slot& slot, ID3D11Texture2D* src, bool nv12, uint32_t slice,
                                  ID3D11Texture2D** out);
    HRESULT UploadBgraSystem(Slot& slot, const uint8_t* rgb, uint32_t pitch,
                             ID3D11Texture2D** out);
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
    ComPtr<ID3D11ComputeShader> m_csDeblockRows;
    ComPtr<ID3D11Buffer> m_cb;

    Plane m_src[3];
    Plane m_rec[2][3];        // ping-pong: m_rec[m_cur] is being written, m_rec[1-m_cur] is reference
    Plane m_upload[3];
    Plane m_levels;
    Plane m_mbinfo;
    // One word per macroblock row plus a leading sentinel, for the single-dispatch deblocking
    // wavefront. Reset from m_progressReset before every deblocking pass.
    Plane m_progress;
    std::vector<uint32_t> m_progressReset;
    // The pictures in flight, as a ring of m_depth slots: m_slotWrite is the next Submit's, m_slotRead
    // the next Collect's, and m_inFlight how many lie between them.
    std::vector<Slot> m_slots;
    uint32_t m_depth = 1;
    uint32_t m_slotWrite = 0;
    uint32_t m_slotRead = 0;
    uint32_t m_inFlight = 0;
    ComPtr<ID3D11Buffer> m_planeStaging;
    // The stage marks: one timestamp query per mark, grown once and reused for every picture after.
    std::vector<ComPtr<ID3D11Query>> m_marks;
    std::vector<uint32_t> m_markStage;    // which stage the dispatch before mark i belongs to
    std::vector<uint32_t> m_markGroups;   // its thread groups
    uint32_t m_markCount = 0;
    uint32_t m_stageLevel = 0;
    DeblockMode m_deblockMode = DeblockMode::Rows;
    GpuStageProfile m_profile;

    uint32_t m_visW = 0, m_visH = 0;
    uint32_t m_padW = 0, m_padH = 0;
    uint32_t m_widthMb = 0, m_heightMb = 0;
    uint32_t m_cur = 0;
    uint32_t m_lastWritten = 0;   // the m_rec index of the collected picture, for ReadReconstruction
    double m_lastGpuMs = 0.0;
    double m_lastReadbackMs = 0.0;
    double m_lastQueryWaitMs = 0.0;
    double m_lastRecordMs = 0.0;
    double m_lastMapWaitMs = 0.0;
    bool m_lastGpuMsValid = false;
};

} // namespace bc250h264
