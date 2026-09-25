from pathlib import Path
p=Path('scratch/m12/directx-sdk-src/C++/Direct3D10/Instancing10/Instancing.cpp');s=p.read_text();old='''    if (g_BC250Benchmark.End(benchmarkStart))
        g_BC250Benchmark.CheckCapture(D3DX10SaveTextureToFileA(
            pRT, D3DX10_IFF_PNG, g_BC250Benchmark.CapturePath()));''';assert old in s
new='''    if (g_BC250Benchmark.End(benchmarkStart)) {
        ID3D10Texture2D* captureSource = NULL;
        HRESULT captureHr = pRT->QueryInterface(__uuidof(ID3D10Texture2D), (void**)&captureSource);
        fprintf(stderr, "BC250 capture QI=%08lx viewFormat=%u\\n", (unsigned long)captureHr, (unsigned)rtDesc.Format);
        if (SUCCEEDED(captureHr)) {
            D3D10_TEXTURE2D_DESC captureDesc;
            captureSource->GetDesc(&captureDesc);
            fprintf(stderr, "BC250 capture desc=%ux%u format=%u samples=%u usage=%u bind=%u cpu=%u misc=%u\\n",
                captureDesc.Width, captureDesc.Height, (unsigned)captureDesc.Format,
                captureDesc.SampleDesc.Count, (unsigned)captureDesc.Usage, captureDesc.BindFlags,
                captureDesc.CPUAccessFlags, captureDesc.MiscFlags);
            captureSource->Release();
        }
        fflush(stderr);
        g_BC250Benchmark.CheckCapture(D3DX10SaveTextureToFileA(
            pRT, D3DX10_IFF_PNG, g_BC250Benchmark.CapturePath()));
    }'''
s=s.replace(old,new);p.write_text(s)
