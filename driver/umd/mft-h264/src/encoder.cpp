#include "encoder.h"
#include "h264_tables.h"
#include <windows.h>

namespace bc250h264 {

namespace {

struct LevelLimit { uint32_t idc; uint32_t maxFs; uint32_t maxMbps; };
// Annex A, Table A-1, the MaxFS and MaxMBPS columns. Only the levels a desktop capture can need.
const LevelLimit kLevels[] = {
    { 10,    99,    1485 }, { 11,   396,    3000 }, { 12,   396,    6000 },
    { 13,   396,   11880 }, { 21,   792,   19800 }, { 22,  1620,   20250 },
    { 30,  1620,   40500 }, { 31,  3600,  108000 }, { 32,  5120,  216000 },
    { 40,  8192,  245760 }, { 42,  8704,  522240 }, { 50, 22080,  589824 },
    { 51, 36864,  983040 }, { 52, 36864, 2073600 },
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
    const uint32_t visW = cfg.width & ~1u;
    const uint32_t visH = cfg.height & ~1u;
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
    const uint32_t fs = wmb * hmb;
    const uint32_t mbps = static_cast<uint32_t>(static_cast<uint64_t>(fs) * fpsNum / fpsDen);
    sps->levelIdc = 52;
    for (const LevelLimit& l : kLevels) {
        if (fs <= l.maxFs && mbps <= l.maxMbps) {
            sps->levelIdc = l.idc;
            break;
        }
    }

    *pps = PictureParams();
    pps->picInitQp = static_cast<int32_t>(cfg.qpInit);
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

    out.clear();
    if (idr) {
        out.insert(out.end(), m_parameterSets.begin(), m_parameterSets.end());
    }
    EmitNal(out, idr ? 3u : 2u, idr ? kNalSliceIdr : kNalSliceNonIdr, bw.Rbsp());

    m_gpu.SwapReference();
    if (stats != nullptr) {
        stats->keyFrame = idr;
        stats->qp = qp;
        stats->bytes = static_cast<uint32_t>(out.size());
        stats->skippedMbs = sw.SkippedMbs();
        stats->gpuMs = m_gpu.LastGpuMilliseconds();
        stats->cpuMs = NowMs() - tGpu;
        (void)t0;
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
