# M533: native D3D shader writes shared WDDM memory

Run002 passes the native D3D clear and triangle readbacks (4096 exact pixels
for each) and independently reads 4096 exact red pixels through Lock2 on the
original WDDM device. All destruction calls succeed; process exit is zero.
The UMD uses Zink memory objects to import the NT handle into candidate RADV.
Rendering has no CPU image-copy path; CPU readbacks here are test oracles.

Run001 failed the original-allocation readback despite passing the D3D reads.
The diagnostic handle was set using SetEnvironmentVariable but read through
CRT getenv. Replacing the reader with GetEnvironmentVariable selected the import;
run002 explicitly logs "native shared probe import ok". No pixel oracle changed.

Zink now closes its temporary duplicated NT handle after memory allocation,
matching the M532 RADV fix that preserves caller ownership on Win32 imports.
The incremental patch applies after the M531 UMD prototype patch. The ICD is M532.

Tested UMD: CFE5D457AE0881A4A7E4B36A77A5DF8823E910E0A00D8B87071FBC43CE110C5A.
Control: FD8DC4B0ECBD159B637D681453B6ED466C9193CE873550683829105043D951F9.
ICD: 7A9970CA37E94D1224FAB40B77BE2FAC4A076CBEA2695DC8044CEF806A6F735E.
Baseline ICD 9C40083C restored and hash checked after both bounded runs.
No OS or DWM restart; no system UMD registration change.

Limitations: custom-driver loading retains the M531 context-callback bypass.
An app-owned NT handle is passed through a diagnostic environment variable.
This is not pfnAllocateCb runtime allocation, native Present, asynchronous
cross-device synchronization, DWM GPU rendering or M13 acceptance.
