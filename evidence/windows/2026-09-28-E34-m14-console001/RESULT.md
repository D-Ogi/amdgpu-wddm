# M740 - Bounded active-console launch and closure control

Source cffa5d15; stage manifest
A7A0EE2A6620E6C8641837DD4EB8384C04F536E518981BD34C5D0BD6E412C413.
The helper is built /W4 /WX. Original host tests pass (quoting, exit, timeout,
orphan and tree closure); a non-SYSTEM host launch is refused with error5.

On unit A, a 90-second SYSTEM task runs four no-GPU fixtures in active console
session1. The child-recorded session and PID match each helper receipt.
Success exits0; deliberate failure exits41 (helper126); orphan exits0 and its
child is terminated; timed-out tree returns124 after3.018s. All four receipts
report job_empty=true. Test script additionally checks recorded process identity
and start time to reject any surviving root or descendant. No windows are opened.

Overall control passes18.2614551s. Before/after exact CPU171 preflight passes,
unchanged boot/driver generation/epoch; no driver/ICD/UMD file or registry change.
Task cleanup succeeds and subsequent Inspect reports Missing. Lab free.
Raw scratch/m14/console001-ops. Selected state omits OS identity, registry,
module paths and environment; task scripts and manifest make the procedure
reproducible. No native D3D11 Present or image acceptance follows from this test.

This removes the session0 limitation of the next native window control while
retaining Job assignment before resume and the independent supervisor deadline.
Cross-session standard handles are not inherited; child-side scripts own logs.
