// Frame level orchestration: GOP structure, rate control, and the join between the GPU half
// (gpu_pipeline) and the CPU half (h264_cavlc, h264_syntax).

#pragma once
#include <stdint.h>
#include <vector>
#include "gpu_pipeline.h"
#include "h264_syntax.h"
#include "h264_cavlc.h"

namespace bc250h264 {

enum class RateControl : uint32_t {
    Cbr = 0,            // eAVEncCommonRateControlMode_CBR
    PeakConstrainedVbr = 1,
    UnconstrainedVbr = 2,
    Quality = 3,        // constant quantiser derived from the quality setting
};

// The lowest quantiser the encoder will use. The forward quantiser clamps every level to +-2047 so
// that level_prefix never has to exceed 15, which Baseline and Main CAVLC do not allow; below about
// qp 6 an Intra_16x16 luma DC can exceed that bound (the worst case is 16 * 16 * 255 * 13107 >> 17 =
// 6528 at qp 0) and the clamp then costs more quality than the finer quantiser buys. Measured on the
// 64x48 ramp: 28.6 dB at qp 0 against 71.0 dB at qp 10.
enum : uint32_t { kQpFloor = 6 };

struct EncoderConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fpsNum = 30;
    uint32_t fpsDen = 1;
    uint32_t meanBitRate = 6000000;
    uint32_t maxBitRate = 0;              // 0: same as meanBitRate
    uint32_t gopSize = 60;                // maximum key frame spacing, in frames
    RateControl rateControl = RateControl::Cbr;
    uint32_t quality = 70;                // 0..100, used by RateControl::Quality
    uint32_t qpInit = 26;
    uint32_t qpMin = 14;
    uint32_t qpMax = 46;
    bool lowLatency = true;
    bool deblocking = false;              // false emits disable_deblocking_filter_idc 1
    // VUI colour description, clause E.2.1 code points, written into the SPS as given. The transform
    // fills these from the input media type; nothing in the encoder converts between colour spaces,
    // so these describe the samples the client hands over. The default is the BT.709 studio-range
    // triple the BGRA import shader produces.
    uint32_t colourPrimaries = 1;         // 1: BT.709
    uint32_t transferCharacteristics = 1; // 1: BT.709
    uint32_t matrixCoefficients = 1;      // 1: BT.709
    bool fullRange = false;               // false: studio range (16..235 luma)
};

// What one picture cost, in the stages a profile has to tell apart. Every millisecond figure is of
// the picture just encoded; nothing here is cumulative.
struct FrameStats {
    bool keyFrame = false;
    uint32_t qp = 0;
    uint32_t bytes = 0;
    uint32_t skippedMbs = 0;
    // The device's own timestamps around the dispatches. Zero with gpuTimingValid false when the
    // device gave no usable pair - never the previous picture's figure.
    double gpuMs = 0.0;
    bool gpuTimingValid = false;
    // Wall clock of the whole GPU stage as the calling thread sees it: upload, dispatches, the wait
    // for the GPU and the readback. gpuMs is the part of it the GPU was busy with our dispatches.
    double gpuWallMs = 0.0;
    // The part of gpuWallMs spent moving the levels and the macroblock info to the CPU, which is
    // where the thread waits for the GPU (GpuEncoder::LastReadbackMilliseconds).
    double readbackMs = 0.0;
    // The CPU half, after the GPU stage: cpuMs is the whole of it, cavlcMs the entropy coding of the
    // slice (clause 9.2 CAVLC plus the slice header) and nalMs the byte stream assembly, which is
    // the emulation prevention scan of clause 7.4.1.1 plus the NAL and parameter set framing.
    double cpuMs = 0.0;
    double cavlcMs = 0.0;
    double nalMs = 0.0;
};

// Derives the sequence and picture parameter sets from a configuration, without a GPU device. The
// Media Foundation transform needs them at SetOutputType time, before any device is known, for
// MF_MT_MPEG_SEQUENCE_HEADER; Encoder::Initialize uses the same function so the two cannot diverge.
void MakeSequenceParams(const EncoderConfig& cfg, SequenceParams* sps, PictureParams* pps);

class Encoder {
public:
    HRESULT Initialize(ID3D11Device* device, const EncoderConfig& cfg);
    void Shutdown();

    // SPS and PPS as an Annex B byte sequence, for MF_MT_MPEG_SEQUENCE_HEADER.
    const std::vector<uint8_t>& ParameterSets() const { return m_parameterSets; }

    // Encodes one picture. The output is an Annex B access unit; a key frame carries SPS and PPS.
    HRESULT EncodeFrame(const GpuFrameInput& in, bool forceKeyFrame,
                        std::vector<uint8_t>& out, FrameStats* stats);

    GpuEncoder& Gpu() { return m_gpu; }
    const EncoderConfig& Config() const { return m_cfg; }
    uint64_t FrameIndex() const { return m_frameIndex; }

    // What the GPU produced for the picture just encoded. Diagnostics only: the host test recomputes
    // the non-zero counts from the levels and compares them with what the shaders reported, which is
    // the one disagreement between the two halves that a decoder cannot see (a wrong nC context
    // produces a different but still parseable bitstream).
    const std::vector<uint32_t>& LastLevels() const { return m_levels; }
    const std::vector<MbInfo>& LastMbInfo() const { return m_info; }
    const SequenceParams& Sps() const { return m_sps; }

    // Runtime adjustable through ICodecAPI; take effect on the next picture.
    void SetMeanBitRate(uint32_t bps) { m_cfg.meanBitRate = bps ? bps : 1; }
    void SetGopSize(uint32_t frames) { m_cfg.gopSize = frames ? frames : 1; }
    void SetRateControl(RateControl mode) { m_cfg.rateControl = mode; }
    void SetQuality(uint32_t q) { m_cfg.quality = q > 100 ? 100 : q; }
    void SetLowLatency(bool v) { m_cfg.lowLatency = v; }
    void SetQp(uint32_t qp) { m_cfg.qpInit = qp; m_qp = ClampQp(static_cast<int32_t>(qp)); }
    void SetQpRange(uint32_t lo, uint32_t hi) { m_cfg.qpMin = lo; m_cfg.qpMax = hi; }

    static uint32_t ChromaQpFromLuma(int32_t qpY, int32_t chromaQpIndexOffset);

private:
    uint32_t ClampQp(int32_t qp) const;
    void UpdateRateControl(uint32_t frameBytes, bool wasKeyFrame);

    EncoderConfig m_cfg;
    GpuEncoder m_gpu;
    SequenceParams m_sps;
    PictureParams m_pps;
    std::vector<uint8_t> m_parameterSets;
    std::vector<uint32_t> m_levels;
    std::vector<MbInfo> m_info;
    uint64_t m_frameIndex = 0;
    uint32_t m_framesSinceIdr = 0;
    uint32_t m_frameNum = 0;
    uint32_t m_idrPicId = 0;
    uint32_t m_qp = 26;
    int64_t m_virtualBuffer = 0;
    bool m_initialized = false;
};

} // namespace bc250h264
