PROVENANCE: the guide character art of this project, owned by the project owner, published here by the
owner's decision of 2026-10-11.

How the set was made (2026-10-04, directed by the owner): the character came from a generated character
sheet of nine busts, which the owner made with an image model from the owner's own prompt and supplied to
the project. The sheet was stripped of the generator's metadata and ran through a pixel pass, then six
faces were cut out of it, which is what the `sheet-` prefix says. Two further faces, `qwen-hopeful` and
`qwen-surprised`, are separate images of the same character, generated with Qwen Image 2.1 on a local GPU
from one bust of that sheet as the reference, which is what the `qwen-` prefix says. Every face then had
its background removed (ISNet-anime), was trimmed, squared and set on the bottom edge of its square, and
was exported at several sizes. This directory holds the two sizes the application embeds, `@128` for a
100 % to 125 % screen and `@256` from 150 % up.

`sheet-surprised` was cut and matted a second time on 2026-10-11. Its first cut reached into the busts
beside it, and the matte then kept a slab of the sheet background: a grey rectangle with speckles left of
her head and a pale band of the neighbour's hair on the right. The second cut takes a narrower cell that
holds this bust alone, mattes it at four times the size with the same model and exports it the same way,
so only the bust is left. Its sides are cut flat where the hair of the neighbouring busts met it, which is
how the other `sheet-` faces are cut as well.

| File | Expression ids (`Guide.ArtFile`) |
|---|---|
| `sheet-welcome` | `01-wave` |
| `sheet-curious` | `02-curious`, `05-thinking` |
| `qwen-surprised` | `03-surprised` |
| `sheet-wink` | `04-wink` |
| `sheet-surprised` | `06-warning` |
| `qwen-hopeful` | `07-hopeful` |
| `sheet-relaxed` | `08-giggle` |
| `sheet-smile` | `09-thumbs-up` |

Eight files, nine expression ids: two ids share the curious face until the set has a face of its own for
each. The set is stills. It holds no `nagi.<expression>-fNN` frame files, so no build of either program
animates her. `GuidePanel` plays a sequence only where frame files exist.

Each file is an 8-bit RGBA PNG (`IHDR` colour type 6, bit depth 8) of its named size and carries three
chunk types only: `IHDR`, `IDAT` (one or two of them) and `IEND`. No EXIF, no XMP, no C2PA manifest and no
text chunk.

These are the bytes the programs embed. `build.ps1` of this application and of
`tools/win/amdgpu_wddm_setup` compares every file it embeds against this list and refuses a file that is
absent, truncated or changed, so a substituted picture stops the build instead of reaching a package.

| File | Bytes | SHA-256 |
|---|---|---|
| `qwen-hopeful@128.png` | 28569 | `C6CBB6D056AE8268CFF621E2F1F09B241B787196D6D49067BF95744A2CEDB669` |
| `qwen-hopeful@256.png` | 95781 | `27958E5C4881B55FF594745C126AD7A8DCDB6F67474753D05156D4BE9CF8B355` |
| `qwen-surprised@128.png` | 28278 | `7B839B4D9F836564C2DA396AA2D9A1E55C5CF2F3D59D997F5CC04B6384B1FD1C` |
| `qwen-surprised@256.png` | 94901 | `FCB0A6C5F2FDBCDA7CBE054A4A953C58D37A9D5A0D786A3732EAC460568111E1` |
| `sheet-curious@128.png` | 27584 | `0E1FBB44202C71700C52F701F1BC435EBDBA6258271DFA3C79B6FB3B82C58FBB` |
| `sheet-curious@256.png` | 98201 | `7BC28826C492F0F4A057472C4DB8A5B12057CC3E54D3CCC2D36AF940918FB5A1` |
| `sheet-relaxed@128.png` | 18900 | `222F9052F74560B851A1D79C8D5B759AD12A18006DED3C0C4F85ACFE14DFB0DB` |
| `sheet-relaxed@256.png` | 62277 | `26AC99CF6C1593AC242FB0547009B79C2AC86CCFF6AD9BF8226C07524B4C469F` |
| `sheet-smile@128.png` | 27471 | `1F938987E28401A8941DC23B913AEC638C9221479FE9794C229A6F071092841B` |
| `sheet-smile@256.png` | 97533 | `8B1CCC9734083EC277F5BE78673EB39CB820C8F74A96E1509C6C61546AC866E0` |
| `sheet-surprised@128.png` | 19272 | `DA7CF503678CADC76EF49A67CB1324A5C659D726C61432636B4B3557266D1081` |
| `sheet-surprised@256.png` | 67317 | `857EBCEF906B5509EB9348D4D00165E80B1A908D55CF24D4D2A545FA3AEE90E3` |
| `sheet-welcome@128.png` | 25564 | `AEB2D71447079D8B90D91092AF5D271E4837F0E928DE5741F860C48C2A5A2C4B` |
| `sheet-welcome@256.png` | 85610 | `EFC875F10C7A1BBFBB0D537072BD7EA101E0C9D96FA496F87297E289977EA795` |
| `sheet-wink@128.png` | 28163 | `BE263D680A2F103AAC673D23D0C98C5CD4E697CEC5C3BBD3EA578991E99E655F` |
| `sheet-wink@256.png` | 99133 | `79AD890D66D4E016477810244777CFDD90ACCEE4B7742B6EDEC9DBB0B9BD6CFE` |

`build.ps1` embeds the files of this directory as the resources `nagi.<expression>@128.png` and
`nagi.<expression>@256.png`. The control application embeds all nine expressions. The setup window embeds
only the expressions its screens can ask for (`SetupGuide.ArtExpressions`). Both read the
expression-to-file map from `src/Guide.cs`, so the map has one copy.
