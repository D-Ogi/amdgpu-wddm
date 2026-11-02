# atlas-text - glyph atlas oracle (BD-208)

A windowless D3D11 probe for the text path of a Skia renderer on ANGLE's D3D11 back end, as Chrome and CEF (the Steam
client) use it. Skia draws text from an alpha-only atlas that it fills with many small uploads of glyph images. ANGLE
makes `GL_R8` a `DXGI_FORMAT_R8_UNORM` texture and `GL_ALPHA8` a `DXGI_FORMAT_A8_UNORM` texture. A `glTexSubImage2D`
becomes `UpdateSubresource`, or a write map of a staging texture and a `CopySubresourceRegion`. If these uploads are
lost, or the alpha of an A8 texture reads as zero, every text label is empty and images and panels are drawn. That is
the picture of BD-208.

## What it does

Four cases: A8 and R8, each with `update` (UpdateSubresource with a box and a tight row pitch) and `staging` (Map WRITE
of a 512x512 staging texture, then CopySubresourceRegion).

- Each frame uploads 32 glyph images of 3-23 x 5-27 texels into the next free cells of a 512x512 atlas. When the atlas
  is full, packing starts again at the top and overwrites old glyphs.
- After every upload, a full-screen pass reads the whole atlas into an R8G8B8A8 target, with no flush in between. The
  target texel is (Load.a, Sample.a, Load.r): A8 must give (v, v, 0), R8 must give (255, 255, v).
- At the end of each frame, the atlas and the target are read back and compared with a CPU model of the atlas.

`--negative` changes the model of the first glyph of frame 1 without an upload, so a working probe must fail.

```
atlas-text --out result.json [--frames N] [--glyphs N] [--any-adapter] [--negative]
```

Exit 0 pass, 1 fail, 2 usage, 3 setup (no adapter, no device, no shader, no output file). Without `--any-adapter` the
probe uses the first 1002:13FE adapter. The result file names the loaded `amdgpu_wddm_d3d11.dll` and its process id.

## Build

```
pwsh tools\win\atlas-text\build.ps1 [-Out <dir>]
```

clang-cl of LLVM 23.1.4, x86-64-v3, static runtime, `/Brepro`. The script prints the compiler version and the SHA256,
and checks that `--help` exits 0 and an unknown argument exits 2.
