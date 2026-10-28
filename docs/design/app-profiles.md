# Game profiles in files

Status: implemented on bc250-win branch `feat/app-profiles`, on the Mesa branches `feat/app-profiles-driconf*`
(hosted, system, WOW64 and desktop lines), on DXVK branch `feat/app-profiles-dxvk` and on vkd3d-proton branch
`feat/app-profiles-vkd3d`, for tester.34. The host tests pass on the development PC (2026-10-28). No lab trial on
unit A has run yet.

A game profile is a set of driver options for one program. Each user-mode engine has built-in profiles in its code.
Before this change, a profile for a new game needed a new build. Now an administrator adds the profile to a file in
the machine profile directory, and the engine reads it when the program starts. Each file has the format of its
upstream project, so an upstream profile entry goes into the file as it is.

## The directory

`<system drive>\ProgramData\amdgpu-wddm\profiles`, where `<system drive>` is the drive of the Windows folder. The
engines do not read `%ProgramData%` from the environment, because the environment belongs to the program.

| File | Engine | Format |
|---|---|---|
| `drirc.d\*.conf` | The Mesa drivers (RADV) | Mesa drirc XML, as `/etc/drirc.d` on Linux |
| `dxvk.conf` | DXVK (D3D11) | `dxvk.conf`, with each option in an `[executable]` section |
| `vkd3d-proton.conf` | vkd3d-proton (D3D12) | config flags per executable, see below |

The engines read no other file. `README.txt`, which the installer writes, is for people only.

## Access rule

An engine reads the directory, `drirc.d` and a file only when all of these are true:

- The owner is SYSTEM, Administrators or TrustedInstaller.
- No allow entry of the access list gives another account a right that changes the object: write data, append
  data, delete, delete child, write the access list, write the owner, generic write or generic all. Entries that
  only a child inherits, deny entries, CREATOR OWNER and OWNER RIGHTS do not count.
- The object is not a reparse point (a symbolic link or a junction).
- A file is not larger than 1 MiB.

Each engine checks this rule itself, on the handle that it reads from. A refused directory or file gives one log line
with the reason and gives no options. A directory that is not there gives no log line.

A file that an elevated administrator makes is usually owned by Administrators. If an engine refuses a file for its
owner, set the owner from an elevated prompt: `icacls <file> /setowner *S-1-5-32-544`.

## Formats

### Mesa: drirc.d

The engine reads each `*.conf` file of `drirc.d` in the order of its UTF-8 name, after the built-in table. A later
file overrides an earlier one, as on Linux. A directory with more than 256 `.conf` files is refused.

The engine reads an XML subset: UTF-8 or US-ASCII, the XML declaration, comments and a `DOCTYPE` without entity
declarations. Text between elements, CDATA and processing instructions are refused. The elements, their nesting and
their attributes follow `driconf.dtd`, and the engine checks them before it uses the file:

- An `<application>` needs `executable`, `executable_regexp`, `blake3` or `application_name_match`.
- An `<engine>` needs `engine_name_match`.
- Each regular expression must compile, each version range must parse and each hash must have its form.
- An `<option>` needs `name` and `value`.

One error refuses the whole file, and the log line gives its line number. An `<option>` that this driver does not
have is ignored, as upstream does, because one file serves all Mesa drivers. A value that does not parse for its
option leaves the option as it was, with a log line.

### DXVK: dxvk.conf

```
# Witcher 3
[witcher3.exe]
d3d11.samplerAnisotropy = 16
dxgi.maxFrameRate = 60
```

The syntax is that of `dxvk.conf`. A section name is the exact file name of the program, with case. Each option must
be in a section, because the file applies in every process. One error refuses the whole file: an option outside a
section, a line without `=`, a quote that is not closed, text after a value (only a `#` comment may follow) or a line
that `dxvk.conf` would skip without a word.

### vkd3d-proton: vkd3d-proton.conf

Upstream vkd3d-proton keeps its profiles in a table in code (`application_override` in
`libs/vkd3d/device_workarounds.c`). This format is the smallest one that holds an entry of that table:

```
# Halo Infinite
[HaloInfinite.exe]
add = FORCE_RAW_VA_CBV, no_upload_hvv
remove = small_vram_rebar

[ends_with:-Win64-Shipping.exe]
add = no_staggered_submit

[appid:1245620]
add = single_queue
```

- A section names the program as the table does: `[name]` is the exact name (`VKD3D_STRING_COMPARE_EXACT`), and
  `starts_with:`, `ends_with:` and `appid:` (the `SteamAppID` variable) are the other compare modes of the table.
  A name is printable ASCII, without a path and without spaces at its ends.
- `add` and `remove` take a list of names with commas. A name is a `VKD3D_CONFIG` option name, as in the
  `VKD3D_CONFIG` variable, or a flag identifier of `include/private/config_flag_decl.h`, as in the table.
- The lines of all sections that match apply in file order. A later line overrides an earlier one.
- `#` starts a comment.

One error refuses the whole file: an option outside a section, an unknown key, an unknown name, an empty name, a
section header that is not closed, an unknown prefix or a byte that is not printable ASCII.

The file holds config flags only. The feature overrides and the shader quirks of the table stay in code.

## Precedence

For each engine, from the highest to the lowest:

| Engine | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| Mesa | The environment variable with the option name | `drirc.d` files, the later file first | The built-in table | The driver default |
| DXVK | `DXVK_CONFIG` (which also carries the per-application settings of the control application) and `dxvk.conf` in the working folder | `dxvk.conf` of the profile directory | The built-in profiles | The DXVK default |
| vkd3d-proton | `vkd3d-proton.conf` | The built-in table | `VKD3D_CONFIG` and the defaults of the D3D12 shell | - |

Notes:

- The per-application settings of the control application ([per-app-graphics-settings.md](per-app-graphics-settings.md))
  win over the files. In DXVK they arrive through `DXVK_CONFIG`. The Mesa drivers and vkd3d-proton take them from
  their own variables (for example `RADV_TEX_ANISO` and `VKD3D_SAMPLER_ANISOTROPY`), not from a profile option, so a
  profile file cannot change them.
- vkd3d-proton applies its table after `VKD3D_CONFIG`, so upstream already lets the table clear a flag that
  `VKD3D_CONFIG` sets. The file follows the table and can clear a flag that the table adds. A matching section
  turns off the Unreal Engine catch-all, as a matching entry of the table does. `skip_application_workarounds`
  turns off the table and the file.

## Release and lab flavours

The release flavour reads only the machine profile directory. The lab flavour can read another directory from the
environment variable `AMDGPU_WDDM_PROFILE_DIR` (BD-167: a release engine takes no file path from the environment).
`tools/quality/flavour_gate.json` declares the name, and the release flavour tests of each fork refuse it outside
the lab flavour.

The lab flavour also logs each entry that applies:

- Mesa: `amdgpu-wddm drirc: <file> line N: <option>=<value> for driver <driver>, executable <executable>,
  application "<application>", engine "<engine>"`.
- DXVK: `Found profile file: <path>`, then `  line N: <key> = <value>` in the DXVK log.
- vkd3d-proton: `amdgpu-wddm profile: <file> line N: [<section>] add|remove <name>.` at level `info`.

Both flavours log a refusal.

## Installer

Phase 2 of the installer (`tools/release/installer/app-profiles.ps1`) makes the directory and `drirc.d` with this
rule: owner Administrators, no inheritance, SYSTEM and Administrators full control, Users, ALL APPLICATION PACKAGES
and ALL RESTRICTED APPLICATION PACKAGES read and execute. The folders and files below inherit the rule.

`%ProgramData%\amdgpu-wddm` lets users add folders. If a `profiles` folder that the engines would refuse is there
before the install, the installer moves it to `profiles.refused-<UTC time>` and makes a new one. It does not reset
the owner of that folder, because that would make the files of a user trusted. A failure in this step is a warning:
the engines work without the directory, with their built-in profiles.

An uninstall keeps the directory. Its files belong to the administrator, and the next install reads them again.

## Tests

| Test | What it shows |
|---|---|
| Mesa `src/util/tests/driconf_profile_test.py` | The XML subset, the driconf rules, the order of the files, precedence against the built-in table and the environment, refused files, the access rule |
| DXVK `src/util/config/config_profile_test.py` | The parser, a section for its program only, precedence against `DXVK_CONFIG` and the built-in profiles, the access rule |
| vkd3d-proton `libs/vkd3d/profile_file_test.py` | The parser, the compare modes, the order after the built-in table, refused files that change no flag, the access rule |
| bc250-win `tools/release/test-app-profiles.ps1` | The access rule of the installer. Elevated, also the directory, the move of an untrusted folder and a second run |

Each runner builds the release and the lab flavour with clang-cl and runs both.

## Limits

- The Zink, OpenGL and llvmpipe Mesa lines do not read the directory yet.
- Section names compare with case, as upstream does. A program name that is not ASCII cannot be named.
