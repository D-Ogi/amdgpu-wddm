# offer-reclaim

The M14.8 oracle for the DXGI Offer and Reclaim path of the D3D11 shell (BD-283). It is a console program with no
window. It creates a D3D11 device on the BC-250 adapter (vendor 1002, device 13FE) and runs a number of cycles on
four resources:

| Case | Resource | What it covers |
|---|---|---|
| `shared` | A keyed-mutex shared texture | The driver sees it as MISC_SHARED. So the shell is expected to give it a kernel allocation, and Offer and Reclaim then go to the kernel (`OfferAllocationsCb`, `ReclaimAllocations2Cb`). The `kernel=` count of the residency lines shows if they did. |
| `shared-nt` | The same, created with an NT handle | The other kind of shared runtime surface. |
| `swapchain` | The back buffer of a composition swap chain, after a Present | The runtime refuses to offer a swap-chain buffer (E_INVALIDARG on a vendor driver), so this case records that answer. |
| `private` | A texture that the engine keeps for itself | Offer and Reclaim are hints in the shell (control). |

In each cycle the program:

1. Draws a colour that only this cycle uses.
2. Offers the resource and calls `IDXGIDevice3::Trim`, as a suspended app does.
3. Waits 50 ms and reclaims the resource.
4. Reads the colour back when the reclaim says that the content was kept.

Odd cycles use `OfferResources` and `ReclaimResources`. Even cycles use `OfferResources1` with
`DXGI_OFFER_RESOURCE_FLAG_ALLOW_DECOMMIT` and `ReclaimResources1`. A DXGI shared resource can be offered only while
its keyed mutex is held, so the program acquires the mutex around each cycle.

A case passes when all of these are true:

- Every cycle leaves the device in place.
- No reclaim fails.
- Each reclaim either discards the content or keeps exactly the colour drawn before the offer.

A refused offer passes only for the swap chain.

```
offer-reclaim --out result.json [--cycles N] [--any-adapter] [--negative]
```

Exit codes: 0 pass, 1 fail, 2 usage, 3 setup (no adapter, no device, no output file). `--any-adapter` takes the first
hardware adapter, which makes a control on another driver possible. `--negative` makes cycle 1 of every case expect a
colour that it did not draw, so a working program must fail every case in which offers happen.

The program does not show whether the shell sent an offer to the kernel. The shell's `BC250 residency:` lines in the
game log of the process show it (`kernel=` is the number of kernel allocations in the batch).

Build: `pwsh -NoProfile -File tools\win\offer-reclaim\build.ps1 [-Out <dir>]` (clang-cl of LLVM 23.1.4, x86-64-v3).
