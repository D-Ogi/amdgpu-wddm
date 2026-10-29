/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// The FSR 4 identity answer: the policy (docs/design/fsr4-identity.md). Plain C, no kernel header: amd_identity.c
// feeds it the image names it gets and the value the D3D12 shell sends, and writes what it answers into the 608-byte
// UMDRIVERPRIVATE reply. The host test (test/amd_identity_test.c) runs the same source.
//
// AMD's FidelityFX upscaler DLL (amd_fidelityfx_upscaler_dx12.dll 4.x) asks type 0 with exactly 608 bytes and reads
// the family at 0x30 and the external revision at 0x34. Its RDNA 2 INT8 provider accepts family 0x8F with revision
// 0x28-0x4F (Navi 21-24). The BC-250 is family 0x8F with revision 0x84, which no FSR 4 provider accepts. A process
// that is ON gets revision 0x32, the first Navi 22 value (40 CUs, GFX10.3); a process that is OFF gets 0x84.
//
// Precedence, first match wins:
//   1. the image is in the anti-cheat list            OFF  (the shell's value cannot change it)
//   2. the D3D12 shell sent FsrUpgrade                that value
//   3. no image name is known                          OFF  (the list cannot be checked)
//   4. otherwise                                       ON
// A query without a process handle is OFF before any of them.
#pragma once

#define BC250_AMD_IDENTITY_FAMILY_NV 0x8Fu      // FAMILY_NV (addrlib amdgpu_asic_addr.h)
#define BC250_AMD_IDENTITY_EREV_OWN 0x84u       // the BC-250 (AMDGPU_GFX1013_RANGE 0x82-0x85)
#define BC250_AMD_IDENTITY_EREV_NAVI22 0x32u    // the first value of AMDGPU_NAVI22_RANGE (0x32-0x3B)

enum bc250_amd_identity_source {
    BC250_AMD_IDENTITY_SOURCE_DEFAULT = 0,      // a known name, not listed, no value from the shell: ON
    BC250_AMD_IDENTITY_SOURCE_LIST = 1,         // the image is in the anti-cheat list: OFF
    BC250_AMD_IDENTITY_SOURCE_SHELL = 2,        // the D3D12 shell sent FsrUpgrade 0 or 1
    BC250_AMD_IDENTITY_SOURCE_NO_NAME = 3,      // no image name: OFF
    BC250_AMD_IDENTITY_SOURCE_NO_PROCESS = 4,   // the query had no process handle: OFF
    BC250_AMD_IDENTITY_SOURCE_COUNT = 5
};

// The facts of one image name.
#define BC250_AMD_IDENTITY_NAME_KNOWN 1u        // the name has at least one character
#define BC250_AMD_IDENTITY_NAME_LISTED 2u       // its file name is in the anti-cheat list

// The shell's value as the process object keeps it.
#define BC250_AMD_IDENTITY_SHELL_NONE 0u        // nothing was sent, or FsrUpgrade has no value
#define BC250_AMD_IDENTITY_SHELL_OFF 1u         // FsrUpgrade 0
#define BC250_AMD_IDENTITY_SHELL_ON 2u          // FsrUpgrade 1

// The FsrIdentity word of the shell's escape (BC250_ESCAPE_PROCESS_POLICY, the same values as
// BC250_PROCESS_POLICY_FSR_*).
#define BC250_AMD_IDENTITY_FSR_OFF 0u           // FsrUpgrade 0
#define BC250_AMD_IDENTITY_FSR_ON 1u            // FsrUpgrade 1
#define BC250_AMD_IDENTITY_FSR_UNSET 2u         // FsrUpgrade has no value: only the image name counts

// The process record's shell word for one escape: the shell's value in bits 0-7 and the NAME_* bits of the shell's
// image name in bits 8-15. fsr_identity FSR_UNSET gives SHELL_NONE, so the name facts are kept and the default
// applies. Returns 0 and writes nothing for a value above FSR_UNSET.
int Bc250AmdIdentityShellWord(unsigned int fsr_identity, unsigned int image_flags, unsigned int* word);

// The facts of an image name. name: a file name or a full path, chars characters long without a terminating zero
// (the name may have none; a zero inside ends it). Only the part after the last '\' or '/' is compared with the
// list, without case for A-Z. name may be NULL.
unsigned int Bc250AmdIdentityNameFlags(const unsigned short* name, unsigned int chars);

// 1 (ON) or 0 (OFF), and the source of the answer in *source (enum bc250_amd_identity_source).
// name_flags: the NAME_* bits of every name known for the process; shell: BC250_AMD_IDENTITY_SHELL_*.
int Bc250AmdIdentityDecide(int has_process, unsigned int name_flags, unsigned int shell, unsigned int* source);

// The 608-byte reply: zeros, the family at 0x30 and the revision at 0x34, both little-endian 32-bit words.
// bytes must be BC250_AMD_IDENTITY_BYTES; another size gives 0 and writes nothing.
int Bc250AmdIdentityFill(void* out, unsigned int bytes, int on);

// The name of a source, for the log.
const char* Bc250AmdIdentitySourceName(unsigned int source);

// The list, for the log and the tests.
unsigned int Bc250AmdIdentityListCount(void);
const char* Bc250AmdIdentityListEntry(unsigned int index);
