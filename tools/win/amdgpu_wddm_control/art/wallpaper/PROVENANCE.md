PROVENANCE: the Nagi desktop wallpaper, own artwork of the project, upscaled 4x with RealESRGAN_x4plus_anime_6B, the small
tagline re-set in Inter (the brand text face) because the generated letters merged. It is stored XOR-obfuscated.
Published here by its author. The repository and the release package hold no plain image of it.

| File | Bytes | SHA-256 |
|---|---|---|
| `nagi-wallpaper.nwp` | 1506666 | `69E96B66F0FCB58DA00E2CBB6A07B3630B99D32D8FBD895FE346787ECE8258B2` |
| the plain JPEG inside it (3840x2160) | 1506622 | `90A41BB7EFE615BDBFFA2FF3D90A17B99D732553424E83E23B4BC0477607D605` |

The format (`.nwp`, all numbers little-endian):

| Offset | Bytes | Content |
|---|---|---|
| 0 | 8 | the ASCII text `NAGIWP01` |
| 8 | 32 | the SHA-256 of the plain JPEG |
| 40 | 4 | the length of the plain JPEG, unsigned |
| 44 | that length | `plain[i] XOR key[i % 32]` |

The key is the SHA-256 of the ASCII text `Nagi's AMD Graphics Infrastructure`
(`8B8AF7BEFE513BE505997B1438A98FC294DD91E17783FF14FB24E08171C13CE6`). This is an obfuscation, not a protection.
The decoder of the control application (`src/Wallpaper.cs`, `NagiWallpaperFile.Decode`) refuses a file whose magic,
length or SHA-256 is not right, and its unit tests pin the two hashes of this table.

The release package ships this file as `payload/control/nagi-wallpaper.nwp` (`tools/release/build-release.ps1`).
Nagi Control App decodes it into `%LOCALAPPDATA%\amdgpu-wddm\wallpaper\nagi-wallpaper.jpg` of the person it sets
the wallpaper for, and nowhere else.
