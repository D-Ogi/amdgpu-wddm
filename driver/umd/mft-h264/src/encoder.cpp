// SPDX-License-Identifier: MIT
#include "encoder.h"
#include "h264_tables.h"
#include <windows.h>
#include <math.h>

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
    int32_t chromaOffset = cfg.chromaQpIndexOffset;
    if (chromaOffset < -12) { chromaOffset = -12; }
    if (chromaOffset > 12) { chromaOffset = 12; }
    pps->chromaQpIndexOffset = chromaOffset;
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
    m_pending.clear();
    m_frameIndex = 0;
    m_framesSinceIdr = 0;
    m_frameNum = 0;
    m_idrPicId = 0;
    m_virtualBuffer = 0;
    m_complexity = 0.0;
    m_initialized = true;
    return S_OK;
}

void Encoder::DiscardPending()
{
    while (!m_pending.empty()) {
        const HRESULT hr = m_gpu.Collect(m_levels, m_info);
        m_pending.erase(m_pending.begin());
        if (FAILED(hr)) {
            // The device is in no state to be drained picture by picture. The pending list is cleared
            // anyway, so that nothing later waits for a picture that will never arrive.
            m_pending.clear();
            break;
        }
    }
}

void Encoder::Shutdown()
{
    if (m_initialized) {
        DiscardPending();
    }
    m_pending.clear();
    m_gpu.Shutdown();
    m_initialized = false;
}

void Encoder::UpdateRateControl(uint32_t frameBytes, uint32_t frameQp, bool idr)
{
    if (m_cfg.rateControl == RateControl::Quality) {
        return;
    }
    const int64_t targetBits =
        static_cast<int64_t>(m_cfg.meanBitRate) * m_cfg.fpsDen / m_cfg.fpsNum;
    if (targetBits <= 0) {
        return;
    }
    // The leaky bucket: how many bits the stream is ahead of its budget. The key frame is charged like
    // any other picture, because the budget is a mean over the stream and its cost is part of that mean;
    // the allocation that makes it worth more is its own lower quantiser in SubmitFrame.
    m_virtualBuffer += static_cast<int64_t>(frameBytes) * 8 - targetBits;
    const int64_t limit = static_cast<int64_t>(kRcBufferFrames) * targetBits;
    if (m_virtualBuffer > limit) { m_virtualBuffer = limit; }
    if (m_virtualBuffer < -limit) { m_virtualBuffer = -limit; }

    // What the content costs, in bits times the quantiser step that produced them. The H.264 quantiser
    // step doubles every six values of qp, and a picture's bits fall roughly in proportion to the step,
    // so bits * 2^(qp/6) is nearly constant for one kind of content and is the one number from which the
    // quantiser the budget needs can be read directly. Only P pictures feed it: an I picture of the same
    // content costs several times as much at the same quantiser, and mixing the two would make every
    // group of pictures begin by quantising its P pictures as if they were key frames.
    //
    // The earlier version had no such estimate. It added two quantiser steps per frame of bucket
    // occupancy to the previous picture's quantiser, which integrates an integral: at 720p and
    // 6 Mbit/s the key frame's 2.1x overshoot drove the quantiser from 23 to 42 over thirteen pictures,
    // every one of them 20 to 30 % under its own budget, and then back down to 28 over ten more.
    // Measured on the development PC: 10.7 dB of luma PSNR lost at the bottom of that swing, and 6 % of
    // the asked-for rate left unspent. A plain integral controller of gain one fixed the swing but
    // reached the quantiser the content needed only after sixty pictures, and spent all of them above
    // the asked-for rate (+10.6 % over the first two seconds at 720p and 6 Mbit/s). Raising its gain
    // bought the rate back and paid for it in quality, because a fast integral hunts: at gain 16 the
    // rate landed within 0.8 % and the luma gap to the inbox encoder fell by 2.7 dB.
    if (!idr) {
        const double k = static_cast<double>(frameBytes) * 8.0 *
                         pow(2.0, static_cast<double>(frameQp) / 6.0);
        m_complexity = (m_complexity > 0.0)
                           ? m_complexity + (k - m_complexity) * kRcComplexityWeight
                           : k;
    }
    // The open loop part: the quantiser at which this content costs the budget. Until the first P
    // picture of the stream has been coded there is no estimate and the configured quantiser stands in.
    const double fullness = static_cast<double>(m_virtualBuffer) / static_cast<double>(targetBits);
    const double gain = kRcGain * static_cast<double>(m_cfg.rcGainScale) / 100.0;
    double want = static_cast<double>(m_cfg.qpInit);
    if (m_complexity > 0.0) {
        want = 6.0 * log2(m_complexity / static_cast<double>(targetBits));
    }
    // The closed loop part: one quantiser step per frame of bits the stream is ahead of its budget,
    // counted only beyond a dead band, because the open loop part already settles where a picture costs
    // its budget and what is left for the bucket to correct is a persistent error, not the ordinary
    // picture to picture variation. Inside the band the quantiser follows the content alone. The band is
    // what a key frame costs over a P picture, so a group of pictures is not made to pay its key frame
    // back within itself: at 720p and 6 Mbit/s that key frame is 1.15 frames of budget, and repaying it
    // picture by picture cost 1.18 dB of luma for 3.0 % of rate. Measured on the development PC over 60
    // pictures, before the search dials below were settled: with the gain acting on every frame of
    // occupancy the stream delivered 100.3 % of the asked rate at -2.88 dB of luma against the inbox
    // encoder, and with it not acting at all 103.3 % at -1.70 dB, against an inbox encoder that itself
    // delivered 106.7 % of the same asked rate. Beyond the band the gain is one full step per frame, so a
    // stream whose content the open loop keeps mispredicting is pulled back inside it.
    double excess = 0.0;
    if (fullness > kRcDeadBandFrames) {
        excess = fullness - kRcDeadBandFrames;
    } else if (fullness < -kRcDeadBandFrames) {
        excess = fullness + kRcDeadBandFrames;
    }
    want += gain * excess;
    // No picture's quantiser moves further than this from the one before it. The open loop part assumes
    // the bits fall exactly in proportion to the quantiser step; on content where they fall faster it
    // would overshoot, and this is what keeps that overshoot from reaching the eye. The step is measured
    // against the quantiser of the P picture series, not against the retired picture's own value, so a
    // key frame's three steps of allocation do not count as movement.
    const int32_t prev = static_cast<int32_t>(m_qp);
    int32_t next = static_cast<int32_t>(llround(want));
    if (next > prev + static_cast<int32_t>(kRcMaxStep)) { next = prev + static_cast<int32_t>(kRcMaxStep); }
    if (next < prev - static_cast<int32_t>(kRcMaxStep)) { next = prev - static_cast<int32_t>(kRcMaxStep); }
    m_qp = ClampQp(next);
}

HRESULT Encoder::SubmitFrame(const GpuFrameInput& in, bool forceKeyFrame)
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
    gp.lambda = (1u + (qp / 8u)) * m_cfg.lambdaScale / 100u;
    if (gp.lambda == 0) {
        gp.lambda = 1u;   // zero would make every vector free and the search would wander
    }
    // Slack for snapping a vector to zero, in SAD over 256 samples. Scaled with the quantiser because
    // at a coarse quantiser a near-match costs nothing extra in residual bits.
    gp.skipBias = (32u + qp * 12u) * m_cfg.skipBiasScale / 100u;
    gp.deblockIdc = m_cfg.deblocking ? 0u : 1u;

    const double t0 = NowMs();
    HRESULT hr = m_gpu.Submit(in, gp);
    if (FAILED(hr)) {
        return hr;
    }

    Submitted s;
    s.idr = idr;
    s.qp = qp;
    s.deblockIdc = gp.deblockIdc;
    s.frameNum = idr ? 0u : m_frameNum;
    s.idrPicId = m_idrPicId & 1u;
    s.submitMs = NowMs() - t0;
    m_pending.push_back(s);

    // The bitstream order counters move on now, not when the picture is retired: the next picture's
    // frame_num follows this one's in bitstream order whether or not this one has been coded yet.
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

HRESULT Encoder::RetireFrame(std::vector<uint8_t>& out, FrameStats* stats)
{
    if (!m_initialized) {
        return E_UNEXPECTED;
    }
    if (m_pending.empty()) {
        return E_NOT_VALID_STATE;
    }
    const Submitted s = m_pending.front();
    m_pending.erase(m_pending.begin());
    const bool idr = s.idr;
    const uint32_t qp = s.qp;

    const double t0 = NowMs();
    HRESULT hr = m_gpu.Collect(m_levels, m_info);
    if (FAILED(hr)) {
        return hr;
    }
    const double tGpu = NowMs();

    SliceParams slice;
    slice.idr = idr;
    slice.pSlice = !idr;
    slice.frameNum = s.frameNum;
    slice.idrPicId = s.idrPicId;
    slice.sliceQp = static_cast<int32_t>(qp);
    slice.disableDeblockingFilterIdc = s.deblockIdc;

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

    if (stats != nullptr) {
        stats->keyFrame = idr;
        stats->qp = qp;
        stats->bytes = static_cast<uint32_t>(out.size());
        stats->skippedMbs = sw.SkippedMbs();
        stats->gpuMs = m_gpu.LastGpuMilliseconds();
        stats->gpuTimingValid = m_gpu.LastGpuTimingValid();
        // Both halves of the GPU stage of this picture: the recording, which happened in SubmitFrame
        // and at a depth above one overlapped the GPU work of the picture before it, and the collect.
        stats->gpuWallMs = s.submitMs + (tGpu - t0);
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

    // The one decision that belongs to the retired picture rather than to the submitted one, because
    // it is the only one that needs the byte count. At a pipeline depth above one the pictures already
    // in flight were quantised before this, so the loop's feedback is that many pictures older.
    UpdateRateControl(static_cast<uint32_t>(out.size()), qp, idr);
    return S_OK;
}

HRESULT Encoder::EncodeFrame(const GpuFrameInput& in, bool forceKeyFrame,
                             std::vector<uint8_t>& out, FrameStats* stats)
{
    HRESULT hr = SubmitFrame(in, forceKeyFrame);
    if (FAILED(hr)) {
        return hr;
    }
    return RetireFrame(out, stats);
}

} // namespace bc250h264
