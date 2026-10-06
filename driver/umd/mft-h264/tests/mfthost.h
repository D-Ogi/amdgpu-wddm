// SPDX-License-Identifier: MIT
// Shared declarations of the host test. Nothing here ships; this is the test harness only.

#pragma once
#include "encoder.h"
#include "mft_h264.h"
#include "mft_register.h"
#include <string>
#include <vector>

namespace bc250h264 {
namespace test {

struct Options {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t frames = 120;
    uint32_t qp = 26;
    uint32_t bitrate = 6000000;
    uint32_t gop = 60;
    uint32_t fps = 30;
    bool deblock = false;
    bool gpuSource = false;         // draw the picture on the device instead of generating it on the CPU
    // --nv12-sys: hand the synthetic picture over as NV12 in system memory, on a stride wider than
    // the picture, instead of as three tightly packed planes. That is the shape a software capture
    // source delivers, and it is the one import path with no oracle of its own otherwise.
    bool nv12sys = false;
    bool noHwTransforms = false;    // --no-hw-transforms: clear MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS
    bool still = false;             // repeat picture 0 so that P_Skip and mb_skip_run are exercised
    bool verbose = false;
    // --timing-skip N: the first N pictures stay in the stream, in the decoder oracle and in every
    // correctness check, and leave the timing averages. Two reasons, both measured: the first picture
    // of a run is an I picture and runs other shaders than the P pictures after it, and a GPU whose
    // clock is at its idle point ramps during the first pictures of a short run. On unit A the DPM
    // idles at 500 MHz of 1500, so a six-picture case can report twice the cost of the same work in a
    // longer one. The count stays in the output next to the averages, so a reader sees what was left
    // out rather than having to trust a round number.
    uint32_t timingSkip = 0;
    int32_t probeX = -1;            // --probe X Y: print this Cb column of every picture
    int32_t probeY = -1;
    RateControl rc = RateControl::Quality;
    // --out <directory>. The default is the working directory: no test output goes anywhere a
    // caller did not name, and nothing of this test ever lands on drive C:.
    std::wstring outDir = L".";
};

double NowMs();

// The device the GPU stages of this test encode on. The shipped transform creates a device on the
// BC-250 adapter and on no other one (gpu_pipeline.cpp), so a test run on a development PC has to
// bring its own device, exactly as a Media Foundation client does through IMFDXGIDeviceManager.
// Prefers 1002:13FE and takes the first hardware adapter when the BC-250 is not in the computer.
HRESULT CreateTestDevice(ID3D11Device** device);

// A picture in I420, visible size, tightly packed. The deterministic CPU twin of testpattern.hlsl:
// not pixel identical to it (the shader works in float and in BGR), but the same kind of content.
struct Picture {
    std::vector<uint8_t> y, cb, cr;
    uint32_t width = 0, height = 0;
    void Allocate(uint32_t w, uint32_t h);
};
void MakeSyntheticPicture(Picture& p, uint32_t frame);

// Draws testpattern.hlsl into a BGRA texture on a given device, one frame at a time. With
// arraySize > 1 the texture is a texture array and the draw goes into slice `slice`, which is the
// shape Game Bar, the Windows frame server and Windows.Graphics.Capture hand to an encoder.
class Pattern {
public:
    HRESULT Initialize(ID3D11Device* device, uint32_t width, uint32_t height,
                       uint32_t arraySize = 1, uint32_t slice = 0);
    HRESULT Draw(uint32_t frame);
    ID3D11Texture2D* Texture() const { return m_tex.Get(); }
    uint32_t Slice() const { return m_slice; }

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11Texture2D> m_tex;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_cb;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_slice = 0;
};

// An NV12 texture, optionally one slice of a texture array, filled from MakeSyntheticPicture. NV12
// is what a capture pipeline actually delivers, so this is the input shape the encoder meets in
// Game Bar and in Windows Camera.
class Nv12Source {
public:
    HRESULT Initialize(ID3D11Device* device, uint32_t width, uint32_t height,
                       uint32_t arraySize = 1, uint32_t slice = 0);
    // Writes picture `frame` into the slice and keeps a copy of it for the comparison afterwards.
    HRESULT Write(uint32_t frame);
    ID3D11Texture2D* Texture() const { return m_tex.Get(); }
    uint32_t Slice() const { return m_slice; }
    const Picture& LastPicture() const { return m_pic; }
    // The D3D11_BIND_* flags the driver accepted for this texture.
    unsigned int BindFlags() const { return m_bindFlags; }

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<ID3D11Texture2D> m_tex;
    Picture m_pic;
    std::vector<uint8_t> m_chroma;
    uint32_t m_width = 0, m_height = 0;
    uint32_t m_arraySize = 1, m_slice = 0;
    unsigned int m_bindFlags = 0;
};

// The inbox H.264 decoder MFT, used as the oracle: whatever it reconstructs is by definition what a
// conformant decoder reconstructs, so our own reconstruction has to match it sample for sample.
class H264Decoder {
public:
    HRESULT Initialize(uint32_t width, uint32_t height);
    void Shutdown();
    // Feeds one Annex B access unit and collects whatever pictures come out.
    HRESULT Feed(const uint8_t* data, size_t size, int64_t timeHns);
    HRESULT Drain();
    // Decoded pictures in output order, I420, visible size.
    std::vector<Picture>& Pictures() { return m_pictures; }
    const std::wstring& LastError() const { return m_lastError; }

private:
    HRESULT PullAll();
    ComPtr<IMFTransform> m_mft;
    std::vector<Picture> m_pictures;
    std::wstring m_lastError;
    uint32_t m_width = 0, m_height = 0;
    bool m_providesSamples = false;
    uint32_t m_outputBytes = 0;
};

// Peak signal to noise ratio of one plane, in dB. Returns 99.0 for an exact match.
double PlanePsnr(const uint8_t* a, const uint8_t* b, size_t count);

int RunSelfTest();
int RunEncode(const Options& o);
int RunMft(const Options& o);
int RunCompare(const Options& o);
int RunSinkWriter(const Options& o);
int RunTextureInput(const Options& o);

} // namespace test
} // namespace bc250h264
