// SPDX-License-Identifier: MIT
#include "encoder.h"
#include "h264_tables.h"
#include <windows.h>

namespace bc250h264 {

namespace {

struct LevelLimit { uint32_t idc; uint32_t maxFs; uint32_t maxMbps; uint32_t maxBrKbps; };
// Annex A, Table A-1: the MaxFS, MaxMBPS and MaxBR columns, in ascending order. MaxBR is in units of
// 1000 bit/s for the Baseline, Constrained Baseline, Main and Extended profiles (cpbBrVclFactor
// 1000), which is the only family this encoder emits. All three columns matter: levels 1.3 and 2.0
// share MaxFS and MaxMBPS and differ only in MaxBR, as do 4.0 and 4.1, so a table without MaxBR
// declares a level the stream can exceed, and a hardware decoder is entitled to refuse that stream.
const LevelLimit kLevels[] = {
    { 10,    99,    1485,     64 }, { 11,   396,    3000,    192 },
    { 12,   396,    6000,    384 }, { 13,   396,   11880,    768 },
    { 20,   396,   11880,   2000 }, { 21,   792,   19800,   4000 },
    { 22,  1620,   20250,   4000 }, { 30,  1620,   40500,  10000 },
    { 31,  3600,  108000,  14000 }, { 32,  5120,  216000,  20000 },
    { 40,  8192,  245760,  20000 }, { 41,  8192,  245760,  50000 },
    { 42,  8704,  522240,  50000 }, { 50, 22080,  589824, 135000 },
    { 51, 36864,  983040, 240000 }, { 52, 36864, 2073600, 240000 },
};

double NowMs()
{
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * static_cast<double>(t.QuadPart) / static_cast<double>(f.QuadPart);
}

} // namespace

uint32_t Encoder::ChromaQpFromLuma(int32_t qpY, int32_t chromaQpIndexOffset)
{
    int32_t qPi = qpY + chromaQpIndexOffset;
    if (qPi < 0) {
        qPi = 0;
    }
    if (qPi > 51) {
        qPi = 51;
    }
    if (qPi < 30) {
        return static_cast<uint32_t>(qPi);
    }
    return kChromaQpFromQpi30[qPi - 30];
}

uint32_t Encoder::ClampQp(int32_t qp) const
{
    const int32_t lo = static_cast<int32_t>(m_cfg.qpMin);
    const int32_t hi = static_cast<int32_t>(m_cfg.qpMax);
    if (qp < lo) { qp = lo; }
    if (qp > hi) { qp = hi; }
    if (qp < static_cast<int32_t>(kQpFloor)) { qp = static_cast<int32_t>(kQpFloor); }
    if (qp > 51) { qp = 51; }
    return static_cast<uint32_t>(qp);
}

void MakeSequenceParams(const EncoderConfig& cfg, SequenceParams* sps, PictureParams* pps)
{
    // The configured size as it stands: every caller has already refused an odd one
    // (IsCodableFrameSize), because the SPS crop cannot express it. Masking it down here was how a
    // 1365x767 negotiation became a 1364x766 bitstream with nothing saying so.
    const uint32_t visW = cfg.width;
    const uint32_t visH = cfg.height;
    const uint32_t wmb = (visW + 15u) / 16u;
    const uint32_t hmb = (visH + 15u) / 16u;
    const uint32_t fpsDen = cfg.fpsDen ? cfg.fpsDen : 1u;
    const uint32_t fpsNum = cfg.fpsNum ? cfg.fpsNum : 30u;

    *sps = SequenceParams();
    sps->widthMb = wmb;
    sps->heightMb = hmb;
    sps->cropRight = wmb * 16u - visW;
    sps->cropBottom = hmb * 16u - visH;
    sps->fpsNum = fpsNum;
    sps->fpsDen = fpsDen;
    sps->colourPrimaries = cfg.colourPrimaries;
    sps->transferCharacteristics = cfg.transferCharacteristics;
    sps->matrixCoefficients = cfg.matrixCoefficients;
    sps->fullRange = cfg.fullRange;
    const uint32_t fs = wmb * hmb;
    const uint32_t mbps = static_cast<uint32_t>(static_cast<uint64_t>(fs) * fpsNum / fpsDen);
    // The declared level has to cover the peak the client asked for, not only the mean, and has to
    // round up: 1 bit/s over a level's MaxBR already needs the next level.
    const uint32_t bps = (cfg.maxBitRate > cfg.meanBitRate) ? cfg.maxBitRate : cfg.meanBitRate;
    const uint32_t kbps = (bps + 999u) / 1000u;
    sps->levelIdc = 52;
    for (const LevelLimit& l : kLevels) {
        if (fs <= l.maxFs && mbps <= l.maxMbps && kbps <= l.maxBrKbps) {
            sps->levelIdc = l.idc;
            break;
        }
    }

    *pps = PictureParams();
    // pic_init_qp stays at 26 whatever quantiser the encoder runs at: every slice carries its own
    // quantiser in slice_qp_delta (clause 7.4.3), so nothing in the bitstream needs this field to
    // follow the configuration. It must not follow it either - MF_MT_MPEG_SEQUENCE_HEADER is built
    // from this at SetOutputType time and a file sink copies it into the container, so a later
    // ICodecAPI quantiser change would leave the container's parameter set disagreeing with the
    // in-band one and every picture would decode at the wrong quantiser.
    pps->picInitQp = 26;
    pps->chromaQpIndexOffset = 0;
    pps->deblockingFilterControlPresent = true;
}

HRESULT Encoder::Initialize(ID3D11Device* device, const EncoderConfig& cfg)
{
    InitDerivedTables();
    m_cfg = cfg;
    if (m_cfg.fpsDen == 0) { m_cfg.fpsDen = 1; }
    if (m_cfg.fpsNum == 0) { m_cfg.fpsNum = 30; }
    if (m_cfg.gopSize == 0) { m_cfg.gopSize = 1; }
    if (m_cfg.meanBitRate == 0) { m_cfg.meanBitRate = 1; }

    HRESULT hr = m_gpu.Initialize(device, m_cfg.width, m_cfg.height);
    if (FAILED(hr)) {
        return hr;
    }

    MakeSequenceParams(m_cfg, &m_sps, &m_pps);
    if (m_sps.widthMb != m_gpu.WidthMb() || m_sps.heightMb != m_gpu.HeightMb()) {
        return E_UNEXPECTED;
    }
    m_parameterSets.clear();
    BuildParameterSetNals(m_parameterSets, m_sps, m_pps);

    m_qp = ClampQp(static_cast<int32_t>(m_cfg.qpInit));
    if (m_cfg.rateControl == RateControl::Quality) {
        // quality 100 is the lowest quantiser we allow, quality 0 the highest.
        const int32_t q = 51 - static_cast<int32_t>(m_cfg.quality * 37u / 100u) - 7;
        m_qp = ClampQp(q);
    }
    m_frameIndex = 0;
    m_framesSinceIdr = 0;
    m_frameNum = 0;
    m_idrPicId = 0;
    m_virtualBuffer = 0;
    m_initialized = true;
    return S_OK;
}

void Encoder::Shutdown()
{
    m_gpu.Shutdown();
    m_initialized = false;
}

void Encoder::UpdateRateControl(uint32_t frameBytes, bool wasKeyFrame)
{
    if (m_cfg.rateControl == RateControl::Quality) {
        return;
    }
    const int64_t targetBits =
        static_cast<int64_t>(m_cfg.meanBitRate) * m_cfg.fpsDen / m_cfg.fpsNum;
    if (targetBits <= 0) {
        return;
    }
    m_virtualBuffer += static_cast<int64_t>(frameBytes) * 8 - targetBits;
    // A key frame is allowed to overshoot by a few frames' worth without pushing the quantiser up
    // for the rest of the group.
    const int64_t floorBits = -2 * targetBits;
    const int64_t ceilBits = (wasKeyFrame ? 8 : 4) * targetBits;
    if (m_virtualBuffer < floorBits) { m_virtualBuffer = floorBits; }
    if (m_virtualBuffer > ceilBits) { m_virtualBuffer = ceilBits; }

    // One quantiser step per 50 % of a frame's budget in the virtual buffer, at most three per frame.
    const double fullness = static_cast<double>(m_virtualBuffer) / static_cast<double>(targetBits);
    int32_t step = static_cast<int32_t>(fullness * 2.0);
    if (step > 3) { step = 3; }
    if (step < -3) { step = -3; }
    m_qp = ClampQp(static_cast<int32_t>(m_qp) + step);
}

HRESULT Encoder::EncodeFrame(const GpuFrameInput& in, bool forceKeyFrame,
                             std::vector<uint8_t>& out, FrameStats* stats)
{
    if (!m_initialized) {
        return E_UNEXPECTED;
    }
    const bool idr = (m_frameIndex == 0) || forceKeyFrame ||
                     (m_framesSinceIdr >= m_cfg.gopSize);

    GpuFrameParams gp;
    gp.intra = idr;
    // An I picture carries the whole group, so it gets a lower quantiser than the P pictures after it.
    const uint32_t qp = idr ? ClampQp(static_cast<int32_t>(m_qp) - 3) : m_qp;
    gp.qpY = qp;
    gp.qpC = ChromaQpFromLuma(static_cast<int32_t>(qp), m_pps.chromaQpIndexOffset);
    // A coarser quantiser tolerates more motion cost; the weights are the usual lambda shape.
    gp.lambda = 1u + (qp / 8u);
    // Slack for snapping a vector to zero, in SAD over 256 samples. Scaled with the quantiser because
    // at a coarse quantiser a near-match costs nothing extra in residual bits.
    gp.skipBias = 32u + qp * 12u;
    gp.deblockIdc = m_cfg.deblocking ? 0u : 1u;

    const double t0 = NowMs();
    HRESULT hr = m_gpu.EncodeFrame(in, gp, m_levels, m_info);
    if (FAILED(hr)) {
        return hr;
    }
    const double tGpu = NowMs();

    SliceParams slice;
    slice.idr = idr;
    slice.pSlice = !idr;
    slice.frameNum = idr ? 0u : m_frameNum;
    slice.idrPicId = m_idrPicId & 1u;
    slice.sliceQp = static_cast<int32_t>(qp);
    slice.disableDeblockingFilterIdc = gp.deblockIdc;

    BitWriter bw;
    bw.Clear();
    WriteSliceHeader(bw, m_sps, m_pps, slice);

    SliceWriter sw;
    sw.Begin(&bw, m_gpu.WidthMb(), m_gpu.HeightMb(), slice.pSlice);
    const uint32_t wmb = m_gpu.WidthMb();
    for (uint32_t mby = 0; mby < m_gpu.HeightMb(); ++mby) {
        for (uint32_t mbx = 0; mbx < wmb; ++mbx) {
            const uint32_t idx = mby * wmb + mbx;
            sw.WriteMb(mbx, mby, m_info[idx], &m_levels[static_cast<size_t>(idx) * kLevelsWordsPerMb]);
        }
    }
    sw.End();
    bw.RbspTrailingBits();
    const double tCavlc = NowMs();

    out.clear();
    if (idr) {
        out.insert(out.end(), m_parameterSets.begin(), m_parameterSets.end());
    }
    EmitNal(out, idr ? 3u : 2u, idr ? kNalSliceIdr : kNalSliceNonIdr, bw.Rbsp());
    const double tNal = NowMs();

    m_gpu.SwapReference();
    if (stats != nullptr) {
        stats->keyFrame = idr;
        stats->qp = qp;
        stats->bytes = static_cast<uint32_t>(out.size());
        stats->skippedMbs = sw.SkippedMbs();
        stats->gpuMs = m_gpu.LastGpuMilliseconds();
        stats->gpuTimingValid = m_gpu.LastGpuTimingValid();
        stats->gpuWallMs = tGpu - t0;
        stats->readbackMs = m_gpu.LastReadbackMilliseconds();
        stats->recordMs = m_gpu.LastRecordMilliseconds();
        stats->mapWaitMs = m_gpu.LastMapWaitMilliseconds();
        // The CPU half, and the two stages it is made of. They are measured, not apportioned: the
        // CAVLC stage ends where the last RBSP bit is written, the NAL stage covers the emulation
        // prevention scan and the framing, and cpuMs is the two of them plus whatever lies between.
        stats->cavlcMs = tCavlc - tGpu;
        stats->nalMs = tNal - tCavlc;
        stats->cpuMs = tNal - tGpu;
    }

    UpdateRateControl(static_cast<uint32_t>(out.size()), idr);
    if (idr) {
        m_framesSinceIdr = 1;
        m_frameNum = 1;
        ++m_idrPicId;
    } else {
        ++m_framesSinceIdr;
        m_frameNum = (m_frameNum + 1u) & 0xFFu;   // MaxFrameNum is 256 (log2_max_frame_num_minus4 4)
    }
    ++m_frameIndex;
    return S_OK;
}

} // namespace bc250h264
