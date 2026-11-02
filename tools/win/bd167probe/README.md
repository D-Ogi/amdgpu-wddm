# bd167probe - the register escape of the release and lab flavours (BD-167 fix 9)

BD-167 removed the raw register escapes from the release KMD: `BC250_ESCAPE_READ_REG` and
`BC250_ESCAPE_WRITE_REG` are compiled only into the lab flavour (`driver/kmd/display.c`, the
`AMDGPU_WDDM_LAB_FLAVOUR` block before `GET_INFO`). This probe is the lab control for that. It sends a write to the
installed driver and reads the register back through a second, independent path.

## The register

`GC.SCRATCH_REG0`, BAR5 offset `0x30100`. `build.ps1` takes the offset from `regcalc.py lookup mmSCRATCH_REG0` and
compares it with `BC250_REG_GC_SCRATCH_REG0` of `driver/kmd/regs.generated.h`, which the program includes. The
register must also be on the KMD's write allow list (`g_MmioWriteAllow`, experiment E07), or the build stops. A
value in the register has no function: amdgpu's ring tests write it during initialization, and `tools/diagusb` uses
it for its pattern test with a restore.

## One run

1. The adapter. The program sends `GET_INFO` only to an adapter with PCI identity 1002:13FE, never to the driver of
   another GPU.
2. The witness. `bc250rd.sys` (`tools/win/bc250rd`) is a separate read-only BAR5 reader that does not depend on the
   flavour of bc250kmd. The program reads the register through it before the write (w0), after the write (w1) and
   after the restore (w2).
3. `READ_REG`, then `WRITE_REG` with the pattern. The default pattern is `0xBC250F09`, inverted when the register
   holds it already. When the write was done, a second `WRITE_REG` puts the value before back.

| `--expect` | pass |
|---|---|
| `release` | both escapes answer `UNKNOWN_COMMAND`, and the witness reads the same value before and after, not the pattern |
| `lab` | `READ_REG` gives w0, the write reads back the pattern and the witness sees it, and the restore puts w0 back (w2 = w0) |

A release answer of `REFUSED` or `NOT_ADMIN` is a fail. Those answers come from a runtime gate, and the fix
removes the code itself. The lab path needs `LabDiagnostics = 1` and `EnableMmioWrite = 1` at the start of the
driver (`bc250kmd_cli state set`, apply next-start), test signing on and an elevated caller (`lab_gate.c`,
`mmio.c`). A closed lab path ends in `lab-path-closed` with exit 2.

The last line:

```
BD167 VERDICT result=<r> expect=<e> read=<status> write=<status> restore=<status> w0=.. w1=.. w2=.. pattern=..
```

Exit codes: 0 pass, 1 fail (a release that writes or reads, a lab mismatch, a failed restore), 2 the lab path is
closed, 3 no BC-250 adapter or no witness, 4 usage.

## Build and host test

```
pwsh tools\win\bd167probe\build.ps1 -Kits $env:BC250_ROOT\toolchain\nuget [-Out <dir>]
pwsh tools\win\bd167probe\test-host.ps1 -Kits $env:BC250_ROOT\toolchain\nuget -Out <dir>
```

clang-cl and lld-link of LLVM 23.1.4 (`tools\build\umd-compiler.ps1`), `/W4 /WX /MT /Brepro`. The host test (quick
gate `bd167probe`) builds twice into two folders and wants one SHA-256. It runs `--self-test`, ten made-up runs
through the verdict rules, and the usage errors. On a PC without the BC-250 GPU it also runs one real run, which
must stop at `no-adapter` before any escape.

## Lab arm

On the installed release flavour, with both lab settings set to 1 and the driver restarted, the stronger form of
the release check:

```
bd167probe.exe --expect release
```

On the lab flavour of the same package, with the same settings and a restart:

```
bd167probe.exe --expect lab
```

Afterwards `state clear LabDiagnostics` and `state clear EnableMmioWrite`, then a restart. The probe changes the
register for the time between the two writes only. It does not exercise the CP privilege check of a user IB:
`SCRATCH_REG0` is a UCONFIG register, which a user IB may write with `SET_UCONFIG_REG` (`tools/win/kmtprobe`
`--scratch` does that), so it cannot raise the privileged-register fault of ih.c.
