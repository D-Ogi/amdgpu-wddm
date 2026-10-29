# FSR 4 identity answer

This page is the contract of the per-process answer to AMD's 608-byte identity query. With this answer, a game
that ships AMD's FSR 4 upscaler DLL can select the RDNA 2 INT8 path of that DLL on the BC-250. The control
application shows the setting as "Allow FSR 4 in games that include it".

## Which games get FSR 4

- A game that ships the 4.x `amd_fidelityfx_upscaler_dx12.dll` (4.1.2 analysed) gets the provider
  `ffxProvider_FSR4_Int8_Navi2` of its own DLL.
- A game that ships only an FSR 3.1.x upscaler DLL keeps FSR 3.1. Its own files have no FSR 4 provider, and the
  driver adds none.
- A game in the anti-cheat list (below) keeps FSR 3.1, whatever the setting.
- A game with `FsrUpgrade` 0 keeps FSR 3.1.

## What the FSR DLL reads

The upscaler DLL 4.1.2 asks `D3DKMTQueryAdapterInfo` type 0 (`KMTQAITYPE_UMDRIVERPRIVATE`) with exactly 608 bytes.
When the call succeeds and the dword at 0x30 is not 0, the DLL reads:

| Offset | Field | ON | OFF |
|---|---|---|---|
| 0x30 | family | 0x8F (`FAMILY_NV`) | 0x8F |
| 0x34 | external revision | 0x32 (first value of the Navi 22 range) | 0x84 (the BC-250) |

All other bytes are 0. The DLL checks no signature, size or vendor field in the block. Its RDNA 2 INT8 gate accepts
family 0x8F with a revision from 0x28 to 0x4F (Navi 21 to Navi 24). The revision 0x84 fails all four FSR 4 gates,
so the DLL selects FSR 3.1.6. That is also the result when the KMD refuses the query, as it did before this change.

The revision 0x32 is Navi 22: 40 compute units and GFX10.3, the Navi 2x part that is nearest to the BC-250. The
upscaler DLL uses the revision only in its gates. The frame-generation DLL has the same reader, and its only user
compares the family with 0x98, so family 0x8F has no effect there. AGS 5.4.2, 6.0.1, 6.2.0 and 6.3.0 parse this
query only after `STATUS_BUFFER_TOO_SMALL` and ignore a successful 608-byte answer. The AGS 6.3 host test
(`tools/build/test-umd-adl-ags.ps1`, case `--kmt-fixture`) shows this: AGS asks 608 bytes first, stops after the
answer, and gives the same device list as before. Before this change it asked 17248 bytes next, which the KMD
refuses.

Not measured: third-party monitor tools (GPU-Z, HWiNFO and similar). If such a tool asks this 608-byte block in a
process that is ON, it shows Navi 22. These tools usually read ADL, not this query.

## Our own readers

No reader of this project asks 608 bytes. RADV, the HIP runtime and the compute path ask 1472 bytes. The D3D12 and
D3D11 shells and the router ask 1520 bytes. They get the `BC25` caps blob as before, with family 0x8F at 0x20 and
revision 0x84 at 0x18. The 608-byte answer is a separate block, not a prefix of the caps blob: at 0x30 the caps
blob holds a field that is not 0.

## Precedence

The KMD decides for each query. The first rule that matches gives the answer:

1. The query has no KMD process handle (`DXGKARG_QUERYADAPTERINFO.hKmdProcessHandle` is NULL): OFF.
2. The image is in the anti-cheat list: OFF. The setting cannot change this.
3. The D3D12 shell sent `FsrUpgrade`: that value.
4. The KMD knows no image name for the process: OFF, because it cannot check the list.
5. Otherwise: ON.

The KMD checks the list against two names: dxgkrnl's process name (`DXGKARG_CREATEPROCESS.pProcessName`) and the
image name that the shell sends. A match on either name is a match. The compare uses only the part after the last
`\` or `/`, and ignores case for A-Z. The WDK does not say if dxgkrnl's name is a full path, a file name or a
shortened name (for example the 15 characters of the EPROCESS image file name). With a shortened name,
`FortniteClient-Win64-Shipping.exe` and `EscapeFromTarkov.exe` do not match. Thus the list must not depend on that
name alone: the shell sends its full image name at every device creation, also when the setting has no value. The
lab log of the KMD records the length and the form of dxgkrnl's name, so that the form is measured.

## The setting

`FsrUpgrade` is a per-application graphics setting (per-app-graphics-settings.md): REG_DWORD 0 or 1, in
`HKLM\SOFTWARE\amdgpu-wddm\Graphics` for all games and in `...\Graphics\Applications\<image>` for one game. The
environment variable `AMDGPU_WDDM_FSR_UPGRADE` has priority over both keys. No value means ON. The control
application writes the value on the Graphics page and on the page of each game.

The KMD reads no registry value for this setting. It opens only its own driver keys (driver package isolation).
The D3D12 shell resolves the setting and sends it to the KMD.

## The escape

The D3D12 shell sends `BC250_ESCAPE_SET_PROCESS_POLICY` (38, `BC250_ESCAPE_PROCESS_POLICY`, 176 bytes, ABI 1) at
every device creation (`driver/umd/d3d12/fsr-identity.h`):

- `FsrIdentity` is the value of `FsrUpgrade` (`BC250_PROCESS_POLICY_FSR_OFF` 0, `_ON` 1), or
  `BC250_PROCESS_POLICY_FSR_UNSET` (2) when the setting has no value. With UNSET the KMD records only the facts of
  the image name, and rules 4 and 5 apply.
- It sends the escape after each device exists. The escape needs the KMD process object (`hKmdProcessHandle`),
  which dxgkrnl creates with the first device of a process. dxgkrnl ends that object when the last adapter handle
  of the process closes. A game that closes a probe device and opens its real device gets a new object, so the shell
  sends the escape again at each device creation. The escape writes the same state each time.
- It opens the adapter by its LUID, sends the escape and closes the adapter. It creates no device, context or
  allocation.
- A failure gives a log line (`d3d12-fsr-identity`) and does not stop device creation.

The FSR DLL needs an `ID3D12Device` before it creates its context, so the escape arrives before the DLL's query.
A change of the setting applies at the next start of the game.

The KMD accepts the escape from any user, in both flavours and with any escape flags. The escape changes only the
calling process's own record. It touches no register. It refuses an ABI other than 1, a value above 2, a
`Reserved0` that is not 0, and a call without a process handle. A KMD from before UNSET refuses the value 2. The
shell then logs the refusal and the KMD's own rules apply, as before.

## The anti-cheat list

The list holds the images of games whose anti-cheat software protects the process that would load the FSR DLL.
`driver/kmd/amd_identity_policy.c` holds the list, with a source for each entry:

| Image | Protection |
|---|---|
| `cs2.exe` | VAC. Counter-Strike 2 runs only in its default mode on this project |
| `r5apex.exe`, `r5apex_dx12.exe` | Easy Anti-Cheat |
| `FortniteClient-Win64-Shipping.exe` | Easy Anti-Cheat |
| `TslGame.exe`, `TslGame_BE.exe` | BattlEye |
| `RainbowSix.exe`, `RainbowSix_BE.exe` | BattlEye |
| `EscapeFromTarkov.exe`, `EscapeFromTarkov_BE.exe` | BattlEye |
| `destiny2.exe` | BattlEye |
| `VALORANT-Win64-Shipping.exe` | Riot Vanguard (a D3D11 game, so the entry is only a safety net) |

Call of Duty is not in the list, because no source names its image. For any other game, the person can set
`FsrUpgrade` 0 on the page of that game.

## Log lines

The lab flavour of the KMD writes at most 32 lines. It writes three lines for the first answer of each process. It
writes one line for an escape that changes the record, if the escape has a value, or if only the shell's name is in
the list. The release flavour writes none.

```
fsr4-identity: query image game.exe source default revision 0x32 names 0x1 shell 0
fsr4-identity: dxgkrnl name game.exe ptr 1 chars 8 end 8 at 0 names 0x1
fsr4-identity: dxgkrnl head game.exe first 0x0067
fsr4-identity: shell image game.exe FsrUpgrade 0 source shell revision 0x84 names 0x1
```

The two `dxgkrnl` lines give what `DXGKARG_CREATEPROCESS` held at CreateProcess. The KMD keeps these values in the
process record and writes them at the first answer, so that only processes that ask for the identity use log lines.

- `ptr` is 1 if `pProcessName` was not NULL.
- `chars` is `ProcessNameLength`.
- `end` is the index of the first zero character in the first `chars` characters (`chars` if there is none).
- `at` is the index of the file name in that name (0: no path).
- `names` holds the flags of that name alone.
- `head` holds the first 31 characters from index 0, and `first` holds the value of the first character (0: no
  character).

The KMD reads no character after `chars`.

The D3D12 shell writes one line each time it sends the escape (`FsrUpgrade=unset/unset` when the setting has no
value):

```
d3d12-fsr-identity FsrUpgrade=0/application result=00000000 status=0 answer=0 source=shell names=1 open=00000000 escape=00000000 close=00000000
```

## Files

| File | Part |
|---|---|
| `driver/contract/bc250_caps_query.h` | `BC250_AMD_IDENTITY_BYTES` and the offsets |
| `driver/kmd/amd_identity_policy.h`, `.c` | the list, the precedence and the reply bytes (no kernel header) |
| `driver/kmd/amd_identity.h`, `.c` | the process record, the query answer, the escape and the lab log lines |
| `driver/kmd/wddm.c` | the hooks in `Bc250WddmCreateProcess` and `QueryAdapterInfo` |
| `driver/kmd/display.c` | the escape dispatch in `Bc250Escape` |
| `driver/kmd/bc250kmd_escape.h` | `BC250_ESCAPE_PROCESS_POLICY` |
| `driver/umd/app-settings/app-settings-core.h` | `FsrUpgrade` |
| `driver/umd/d3d12/fsr-identity.h`, `adapter.cpp` | the sender |
| `tools/win/amdgpu_wddm_control/src/GraphicsSettings.cs` | the control in the application |

Tests:

- `driver/kmd/test/run_amd_identity.ps1` (the `amd-identity` check of `tools/quality/quick.ps1`): the list, the
  precedence, the escape's shell word (UNSET, and shortened dxgkrnl names of listed games), the reply bytes and the
  sizes that are not 608.
- `driver/umd/d3d12/fsr-identity-test.cpp` (run by `tools/build/build-umd-d3d12.ps1`): the escape that the shell
  sends, against fake KMT entry points.
- `driver/umd/app-settings/app-settings-test.cpp`: the setting and its precedence.
