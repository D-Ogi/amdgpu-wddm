# M678: stationary CPU composition control

Source b2bbe24; EXE SHA256 32DB787E819F4C23D2A4EAB7125D75BA66DE0C64D2C2985FD540AE446432CC1B.
Run script and manifest are retained here. Build: MSVC /O2 /MT /W4 /WX,
user32/gdi32/dwmapi; no host GUI run. Unit A, KMD164 SYS9B9B99D3,
CPU DWM8220, UMD8279AC7F, registered ICDCF3948D6. Before/after hashes,
boot identity, health15 and 1000MHz/VID116 retained; closure66.25C.

Control7196 started14:32:55.2421251Z. First primary capture passed at
frames0 after paints and DwmFlush. Independent GDI screenshot passed too.
Both contain all26,584 expected RGB pixels: red1,400, overlap6,600,
cyan full client18,584 (608,151)-(792,252), zero mismatches. Independent
host pixel decoding agrees and the per-region RGB hashes match exactly.
Whole-desktop images remain in the private trial archive; their hashes and
selected region results are retained here. No desktop-wide equality is claimed.

An explicit animate marker then produced frames20 and a changed client
rectangle. The freeze acknowledgement is S_OK at frames20, final bounds
(640,175)-(888,308). No dynamic or frozen image correctness was measured in
this CPU handshake control. Heartbeats precede some timer updates, so the
moving receipt's frames20 bounds differ from the later frozen receipt; use
frozen geometry for a final capture, never a live heartbeat as a frame oracle.

Worker finished14:33:05.4386258Z, exit0, child not alive. Original process
identity was rechecked absent and the terminal task removed14:34:05.1478702Z.
CPU DWM8220 and the OS boot were retained. No driver/ICD/config changes.

This repairs the positive-control method, not the historical M672 result.
The cause of its failed moving CPU baseline remains unproven. A fresh GPU
trial with this stable baseline and full CPU-copy evidence is still required;
G0 is not accepted by this measurement.
