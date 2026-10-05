# M681: CPU DWM handshake negative control

Probe sourcec0edf93, SHA2560DC75F8C8BDD05ED332322FD862A6634715A3C769FB0FBB520A35917F51181A4.
Runner helpers0b0ab2b; exact staged scripts and manifest retained. Unit A with
exactKMD164/9B9B99D3, CPU UMD8279AC7F, registered ICDCF3948D6, DWM12796.
No driver/configuration transition, same OS boot10:58:57.500Z and DWM before/after.

Both641x479 windows used --handshake-only --no-open, explicit adapter LUID,
500ms initial pump and1s hold. No source allocation, device, paging queue,
Present context, surface open or Present variants. First PID1444 without GDI
paint, second PID11060 with GDI paint. Both return DWM_S_GDI_REDIRECTION_SURFACE_
BLT_VIA_GDI (0x00263008), format0, null handle, update id0, reported duration0ms.
A0ms tick reading is resolution-limited, not proof of zero execution time.
Ordinals100 and101 resolve; composition/session/console checks are recorded.
The returned code is an observed CPU baseline, not a GPU-present success.
Painting did not change the answer in these two cases. No conclusion about
all windows or the future hosted GPU DWM answer follows from this result.

ETW spans15:13:58.6389437Z to15:14:07.5496418Z, zero lost events/buffers.
Both probe PIDs have zero Present rows; no Blit or CddStandardAllocation287 rows
occur in this interval, and no model3 history row is recorded. Other processes
produce107 Present rows, establishing that the provider captured presentation
activity. History payloads contain39 event171/model7 rows and39 event172/model0
rows; these event counts are not a count of distinct tokens.

Both child identities verified terminal, task removed15:15:35.1768729Z.
Controller done15:14:08.0734315Z exit0. Closure health15,1000MHz/VID116,66.375C,
baseline hashes and DWM/boot retained. No active trial. Raw ETL and archive remain
private under scratch/g0-hosted/redirblt-cpu001; hashes retained. The first collection
script failed locally before cleanup, then cleanup succeeded; a separate pull-only
retry collected the archive without repeating the run or cleanup.

Next discriminator: the same two observations under confirmed hosted GPU DWM.
Actual redirected Present variants and no-CPU-copy acceptance remain separate.
