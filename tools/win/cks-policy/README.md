# cks-policy

`New-CksPolicy.ps1` makes a custom kernel signers policy. This Code Integrity base policy lets Windows load the
amdgpu-wddm kernel driver while Secure Boot is on, with Windows test mode off. Microsoft documents the feature in
[Custom kernel signers for App Control for Business](https://learn.microsoft.com/windows/security/application-security/application-control/app-control-for-business/design/custom-kernel-signers).

The policy belongs to the owner of the Secure Boot keys of the PC, not to the driver package. So the tool is
separate from the installer, and it reads public certificates only. The owner signs the policy with a key that
chains to the PK or KEK of the PC.

```
powershell -NoProfile -File New-CksPolicy.ps1 -UpdateSigner <kek.cer> -KernelSigner <driver.cer> -Out <empty folder>
```

The output folder then holds the policy XML, the unsigned binary policy `<PolicyID>.cip` and `SIGN.txt` with the
`signtool` command. The steps after the signature are in `docs/testing/INSTALL.md`, section "Secure Boot on:
custom kernel signers". The installer finds an active policy of this kind by itself (`Read-KernelTrustRoute` in
`tools/release/installer/common.ps1`).

Test: `powershell -NoProfile -File test.ps1`. It makes two throwaway certificates in memory and checks the policy
that the tool writes for them, with negative controls.
