# The pre-OS TPM

## What it is

The TPM servicer (`driver/tpm`) gives Windows a TPM 2.0 after Windows starts: the stage publishes a CRB device
(`ACPI\MSFT0101`), the filter `tpmserv.sys` polls its page, and the user-mode core `tpmcored.exe` runs libtpms.
That TPM does not exist while the Windows Boot Manager runs. BitLocker unlocks the operating system volume in the
Boot Manager, through `EFI_TCG2_PROTOCOL`, so a TPM protector on `C:` needs a TPM before Windows.

The pre-OS TPM is the same TPM, started earlier. The stage runs libtpms itself, offers `EFI_TCG2_PROTOCOL` with a
TCG event log, measures the boot path, and at `ExitBootServices` hands the TPM state to the Windows core. The core
continues that state. Windows sees one TPM with one set of PCRs from the stage to the desktop.

## What it gives, and what it does not give

- BitLocker on `C:` with a TPM protector unlocks without a prompt when the boot path is unchanged, and asks for the
  recovery key when the measured path changes (another boot manager, another stage).
- It is not a hardware root of trust. The measurements start at the stage, not at the firmware, and the TPM state is
  a file on the EFI system partition. A person who has the disk and the board has the key. The state file is
  encrypted with a key derived from the board identity (see "The state on the EFI system partition"), so the disk
  alone does not give the key.
- A change of the stage image changes PCR 0. A stage update therefore suspends BitLocker for one restart first, the
  same rule that Windows uses for a firmware update.

## The parts

| Part | Where | Job |
|---|---|---|
| The C runtime of the stage | `tools/efi/tpm/crt` | The freestanding C library that libtpms and OpenSSL need: memory from one arena, strings, time, entropy |
| The TPM engine | `tools/efi/tpm/engine` | libtpms and OpenSSL built for the stage target, behind `tpm_engine.h` (start, process, get and set state) |
| The event log | `tools/efi/tpm/src/tl_log.c` | The crypto-agile TCG event log and the final events table (pure code, host tested) |
| The measurements | `tools/efi/tpm/src/tl_measure.c` | What the stage measures, into which PCR, in which order (pure code, host tested) |
| The TCG2 protocol | `tools/efi/tpm/src/tpm_module.c` | The UEFI binding: the module, `EFI_TCG2_PROTOCOL`, the `ExitBootServices` event |
| The handoff record | `driver/tpm/handoff.h` | The format that the stage writes and the filter reads (shared header, host tested on both sides) |
| The filter | `driver/tpm/filter` | Maps the handoff range, gives it to the core once through an IOCTL, then clears it |
| The core | `driver/tpm/core` | Continues the handed state, keeps the state files on the EFI system partition |

The stage module `tpm` (load option `tpm=0` turns it off) runs after the core unlock and before the ACPI repair.

## The sequence of one start

1. **run.** The module reads the state file of the EFI system partition, starts libtpms with it (or manufactures a
   new TPM when no file is there), sends `TPM2_Startup(CLEAR)`, reads the active PCR banks, and installs
   `EFI_TCG2_PROTOCOL` and the final events table. It measures the pre-OS events (next section). It allocates the
   handoff range as `EfiReservedMemoryType` and gives its address to the core of the stage (`stage->tpm_handoff`);
   the ACPI repair describes it in `Device (TPM0)`.
2. **gate.** The module measures the Authenticode digest of the next image into PCR 4 and answers
   `STAGE_GATE_BIND`, so that the bytes that start are the bytes that were measured.
3. **The Boot Manager and the boot loader** use the protocol: `GetEventLog`, `HashLogExtendEvent`, `SubmitCommand`.
   BitLocker unseals its key here.
4. **ExitBootServices.** The event of the module measures the `ExitBootServices` action into PCR 5, writes the
   permanent and the volatile state of libtpms into the handoff range, and stops the protocol. The callback does not
   allocate memory from the firmware: libtpms and OpenSSL allocate from the arena of the stage C runtime.
5. **Windows.** The filter reads the handoff range, the core takes it, sets the two states into libtpms and starts
   without `TPM2_Startup`. `tpm.sys` finds a started TPM, as with a firmware TPM.

## The measurements

The order follows the TCG PC Client Platform Firmware Profile and the edk2 TCG2 driver, so that the Windows log
checks see a familiar log.

| PCR | Event | Data |
|---|---|---|
| 0 | `EV_NO_ACTION` (log only) | The Spec ID event: the algorithms and digest sizes of the active banks |
| 0 | `EV_S_CRTM_VERSION` | The stage version string, UTF-16 |
| 0 | `EV_S_CRTM_CONTENTS` | The Authenticode digest of the stage image |
| 4 | `EV_EFI_ACTION` | "Calling EFI Application from Boot Option" |
| 7 | `EV_EFI_VARIABLE_DRIVER_CONFIG` | `SecureBoot`, `PK`, `KEK`, `db`, `dbx` as `GetVariable` returns them |
| 0-7 | `EV_SEPARATOR` | Four zero bytes |
| 4 | `EV_EFI_BOOT_SERVICES_APPLICATION` | The next image (gate) |
| 5 | `EV_EFI_ACTION` | "Exit Boot Services Invocation" (at `ExitBootServices`) |

Without Secure Boot, BitLocker uses the PCR profile 0, 2, 4, 11. PCR 2 holds only its separator, PCR 11 belongs to
BitLocker.

## The handoff range

One range of `EfiReservedMemoryType` pages: a header (`handoff.h`: magic, version, length, the partition GUID of
the stage volume, the sizes of the two blobs, a SHA-256 over all of it), then the permanent state, then the volatile
state. The ACPI repair writes `Name (BCHO, Package () { address, length })` into `Device (TPM0)`; the filter reads it
with `IOCTL_ACPI_EVAL_METHOD` from the device below it. The range is not in `_CRS`, so the arbiter does not see it.

The filter maps the range cached (it is RAM, written by the processor), checks the header, copies the record for one
`IOCTL_TPMSERV_GET_HANDOFF` of the core, and then overwrites the range with zeros: the state holds the seeds of the
TPM, and no later reader must find them there.

## The state on the EFI system partition

- **Where.** `\EFI\BC250\tpm\state-a.bin` and `state-b.bin` on the volume of the stage. The core finds that volume
  by the partition GUID in the handoff header.
- **Format.** `magic | version | sequence | length | nonce | encrypted blob | tag`. The blob is the permanent state
  of libtpms. AES-256-GCM with a key that both sides derive from the board identity: SHA-256 over a fixed label and
  the SMBIOS system UUID and baseboard serial. The key never goes to a file.
- **Writers.** Only the Windows core writes. It writes on each NV store of libtpms and at its stop, to the file with
  the older sequence, flushes it, and the higher valid sequence wins at the next start. A power cut leaves the
  previous copy.
- **Reader.** The stage reads both files and takes the valid copy with the higher sequence. Pre-OS NV changes (the
  clock, the dictionary attack counter) go to Windows in the handoff and are written by the core.
- **No valid copy.** Both files absent: the stage manufactures a new TPM. One or both files present and none
  valid: the stage does not start a TPM and does not publish the device, and says why. A TPM that starts empty in
  silence would lose every sealed key without a word.

## The failure policy

The stage starts Windows on every path. A TPM that did not start means no protocol and no device; BitLocker then asks
for the recovery key, which is the correct answer to a changed platform. The core without a handoff (stage with
`tpm=0`, or the stage of an earlier version) keeps the behaviour of the servicer: its own `TPM2_Startup`.

## The test plan

- **Host tests.** The event log against the TCG format (and a parser check against a real Windows measured-boot log
  as a positive control), the measurement order, the handoff record (write, check, corrupt, refuse), the state file
  (sequence choice, refusal, key derivation), and the engine: the same objects of the stage target linked into a host
  program run `TPM2_Startup`, `PCR_Extend`, `Create`, `Unseal`, `GetState` and `SetState`, and a resumed TPM answers
  the unseal of a key that was sealed before the handoff.
- **Lab steps.** (1) The stage with the module: the log shows the TPM start and the measurements; `Get-Tpm` in
  Windows is ready, and `TPM2_PCR_Read` of PCR 0 and 4 equals the replay of the Windows measured-boot log. (2) Two
  stage boots give the same PCR 0, 2 and 4. (3) BitLocker: the recovery password to `secrets\`, a TPM protector on
  `C:`, encryption of the used space, then a stage boot that unlocks without a prompt.
