# A8 shared import and GPU sampling on unit A

On Windows 22631, the candidate hosted Zink imports a native D3D11 producer's
A8 texture and samples the expected alpha values on the BC-250 GPU. The baseline
fails at `OpenSharedResource` with `0x887A0005`. BGRA controls pass on both builds.
These are per-process client results, not desktop or Settings acceptance.

## Exact artifacts and method

Client source: `9f1a91233835306ce882b105347489c6971fc84f`.
Both parent and peer executable SHA256:
`E77A46EB52CE8D29DBDBDFE1CED559218212B03BEC9E2AA078F4C1503C480BF8`.

| Arm | Hosted Zink SHA256 | Source |
|---|---|---|
| A | `9D3E6CC6B96F9744B434FBCC0C466D791BEED401B2409DCBEB010E6EA0CF38AE` | installed baseline |
| B | `51C5E537171D1611921B3FBB4B26339AB587CB0D5FBEB2CA1CC3840BAA3B938E` | Mesa candidate `63fd611f` |

Each client log records its process identity, loaded module paths and on-disk
SHA256. This establishes the observed module association and disk artifact;
it is not a hash of loaded memory. The parent selects native `gpu11` and the
separate peer selects hosted `zink11`. The executable bytes are identical in
both processes and in all six arms.

The producer checks `ExtendedResourceSharing` and format support, creates a
DEFAULT shared SRV texture, uploads a row-dependent pattern containing alpha
0, 1, 127, 128, 254 and 255, waits for GPU completion, and exports a legacy KMT
shared handle. The peer opens it, samples through a shader into RGBA8, copies
the result to a separate staging texture, and compares every pixel/channel
against an independently computed oracle. Three resource generations run per
positive case. The A8 shader checks alpha and the expected zero RGB channels.
BGRA exercises a known supported sharing path; the wrong-channel shader is a
negative control for the alpha oracle.

## Results, 2026-11-01

Tags identify arm invocations; they are not elapsed-time measurements or a
cross-machine clock alignment. Per-process timing and controller timing stay
in the supplied records.

| Tag suffix | Arm / case | Readback | Client exit |
|---|---|---|---|
| 061007Z | A / BGRA 704x640 | 3 x 450560 pixels, zero differences | 0 |
| 061135Z | A / A8 704x640 | OpenSharedResource fails `0x887A0005`; no readback | 2 |
| 061221Z | B / BGRA 704x640 | 3 x 450560 pixels, zero differences | 0 |
| 061313Z | B / A8 704x640 | 3 x 450560 pixels, zero differences | 0 |
| 061411Z | B / A8 701x639 | 3 x 447939 pixels, zero differences | 0 |
| 061458Z | B / A8 wrong-channel injection | 375466 / 450560 pixels differ; max delta 255 | 1, expected |

Each controller completes in 7.173 to 13.852 seconds. All recorded guard
samples are safe; the highest reported temperature is 73.1 C. All three
restoration records per arm report `restored=true` with no errors. The raw
client verdict's summary `diff=0/0` on positive runs is not the pixel count;
the three explicit `ALPHA readback-pitch` lines supply those counts.

This narrows the BD-168 investigation: the candidate fixes the exercised A8
import/sample path at the same 704x640 extent observed in Settings diagnostics.
It does not establish that all Settings symptoms share this cause, that DWM
composition/input works, or that CPU Map of an imported resource works. The
imported DEFAULT texture is never mapped; only the separate RGBA staging
texture is mapped. No physical display, Explorer or clock acceptance follows.

## Evidence and redactions

`REDACTIONS.json` binds each immutable raw archive SHA256 and collection receipt
SHA256 to every selected member's original hash, published filename and
published hash. Sixty raw text members and six derived analysis records are
included. Selected records preserve complete client logs, verdicts, assessments,
guards, completion and restoration results. JSON whitespace and text encoding
are normalized to UTF-8/LF; trailing horizontal whitespace is removed from logs.

Absolute trial, Windows and DriverStore package roots become `<trial-root>`,
`<windows>` and `<driver-package>`. Executable/DLL basenames, build hashes,
process lifetime IDs and result data remain. UUID and MAC redaction patterns
are applied; no host name, user path, network address or credential is included.
Binary payloads, runner scripts and registry journals remain in the private raw
archives and are omitted from this public evidence. Original archives and
receipts are unchanged. Analysis JSON is derived and separately hash-bound;
the client logs remain the source for pixel and module observations.
