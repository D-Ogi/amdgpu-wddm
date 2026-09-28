# M765: temporary fourth registration slot does not change the effective DX12 name

The CPU171 baseline from M764 was captured and its exact three-entry
UserModeDriverName REG_MULTI_SZ was extended with the absolute diagnostic DX12
UMD path. Install completed with exact registry kind/value readback and a closed
writer Job. The diagnostic UMD is DE191A9D and KMT probe E9D82C38, as in M764;
full hashes are in stage-manifest.json. Transaction helper source: 01d854ea.

KMT DX9/10/11 queries still succeeded. DX12 still returned 0xC000000D without a
name. The probe independently admitted and closed the explicit diagnostic DLL
through the real KMD contract. The system D3D12 client was skipped because its
name-admission prerequisite failed. This is not a functional D3D12 pass.

The supervisor recorded failed-restored after 22.786104 seconds. It verified
all five child Jobs empty, restored the three original registration values,
and checked the original KMT name behavior. CPU171 binary identities, boot,
DWM identity/modules, health generation/epoch and UMD/ICD registration stayed
unchanged. The scheduled task was subsequently removed; task-closed.txt confirms
Missing. No adapter restart, OS reboot, desktop promotion or ICD change occurred.

This measures failure to refresh the effective DX12 name in the current device
session after this registry write. It does not establish that a fourth entry is
invalid, that a restart would fix it, or that registration is the sole remaining
condition. Next inspect the OS driver-name selection/initialization path offline
before choosing another bounded experiment.

The reused supervisor calls the name-query phase Cpu and the native runtime
phase Gpu; these are phase labels, not changes in desktop composition mode.
Raw pre/postflight inventory stays in scratch/m15/registration001. The selected
comparison omits inventory identifiers and includes only binary hashes, equality
checks, health admission output and temperature. No secret data is included.
