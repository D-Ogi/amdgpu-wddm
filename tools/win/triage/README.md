# triage - first look at a kernel minidump without a debugger

```
python triage.py <minidump> [file of the faulting module]
```

The lab had no kernel debugger for most of this work, and a minidump is small enough to carry off the target
over ssh. This script reads one directly and answers the three questions worth asking before anything else:

- what the bugcheck was, with its four parameters;
- which loaded module contains the faulting address, and at which offset;
- which modules the stack points into, top first, repeats folded - and whether any of ours was loaded at all.

With the faulting module's own file as second argument it also prints the code bytes around the faulting
offset, and says whether the file is the same build as the image in the dump (timestamp and checksum from the
PE header against the values recorded in the dump). Triage dumps rarely carry code pages, so this is the only
way to see the instruction. There are no symbols anywhere in this: offsets only. Use `tools/win/kd/` when a
real debugger is available; this stays useful because it needs nothing but Python.

## What it knows about the file format

`DUMP_HEADER64`: bugcheck code at `0x38`, parameters at `0x40`, dump type at `0xF98`. `TRIAGE_DUMP64` at
`0x2000` as a row of `ULONG` fields; driver entries of `0x90` bytes with the name's string-pool offset at
`+0`, `DllBase` at `+0x38`, `SizeOfImage` at `+0x48`, `CheckSum` at `+0x80`, `TimeDateStamp` at `+0x88`;
string-pool entries are a `ULONG` character count followed by UTF-16.

Every one of those is checked before it is used. The dump type has to be 4 (triage), the signature `TRGD` has
to sit exactly where `TRIAGE_DUMP64.ValidOffset` says it does, the three offsets into the file have to be
inside it, and each driver entry has to look like one (kernel-space page-aligned base, plausible name length).
A dump whose layout differs makes the script stop with a message saying so - it never prints a guess.

## Run on

`092126-43343-01.dmp` from unit A, Windows 11 22621, with `nwifi.sys` from the same machine:

```
bugcheck 0x1e  (0xffffffffc000001d, 0xfffff8027c82b660, 0x0, 0xfffff8027c82b660)
faulting address 0xfffff8027c82b660: nwifi.sys+0xb660
  file matches the loaded image (timestamp 0x5dc1b697/0x5dc1b697, checksum 0xbd320/0xbd320)
our modules loaded: none
```

`kd -z` on the same dump with symbols says `nwifi!NwfReadMsg+0x1a0` in bucket
`0x1E_C000001D_nwifi!NwfReadMsg`, which is the same address: the script's answer is the symbol-free half of it.

Dumps and module files are evidence from a machine and never go into this repository; keep them under
`P:\BC-250\scratch\dumps\`.
