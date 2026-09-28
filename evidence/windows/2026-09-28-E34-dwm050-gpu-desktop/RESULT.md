# M723 - bounded GPU desktop implementation proof, DWM050

Runner5fd5ff5, package manifest70AA544C4006B33EB18E6BB475FD49576008F3F13F3CF45E3A92FCEB06F594B0. KMD170/a3c2d2b/SYS67F02415, hosted UMD92697AE5 and ICDC0CE. [Selected receipts and hashes](observations.json). Full local inputs: scratch/g0-hosted/dwm050-ops. Export omits raw registry/device identifiers.

The whole trial passes in124.910s, with8 acknowledged markers. Native client exits0 after2824 frames; both independent primary readback and composed screenshot pass40712 frozen control pixels each, no mismatch. DWM4944 loads the exact hosted candidates. CPU baseline restored, collector/watchdog terminal, tasks removed. Independent CPU170 preflight passes health15, unchanged boot,67.5C (see receipt for exact reading). No OS reboot or AC cycle.

## G0 evidence matrix

| Requirement | Observed evidence | Limit |
|---|---|---|
| Correct composed image plus independent primary readback | Both40712-pixel controls pass; moving/resizing native window and composition control complete | Frozen and scheduled sampled oracles, not every pixel of every frame |
| GPU execution attributable to DWM composition |4498 DWM4944 DMA pairs, no trace loss/unmatched/pending/duplicate starts; matching submit/completion IDs | DMA alone is combined with the image and Present witnesses |
| Runtime allocations and render-to-Present synchronization |4274 exact allocation/wait/Present/signal joins in one ETW context, independent checkpoint total agrees, presented imports86/90/94 |99 post-boundary events are explicitly outside the joined prefix |
| No steady-state full-frame CPU copy under the agreed bounded list |82 in-render KMD samples zero software blits;24635 reconciled maps, runtime-map count0, no image map spans a boundary; eight live maps are buffers, no pending writer spans | Instrumented scope and positive controls below; not a claim about every possible OS memory writer |

## Agreed no-copy closure list

The accepted187/337 coordination froze closure at(a)-(d), reopening only for concrete contradictory data. This result uses that scope. The later generic request for complete writer coverage in M720 is not an additional acceptance gate.

- (a) Application window uploads: M695 and M720 classify the recurring UpdateSubresource inputs. They are not composed-desktop copies merely because they contain window pixels.
- (b) Positive KMD copy control: M702 observed CPU Blit=1 with the same decoder. WddmPresentBlit is byte-for-byte identical after LF normalization in166 and170 (hash in receipt). DWM050 itself did not exercise this positive arm; its standalone strict same-run-positive analyzer therefore returns non-acceptance. Its zero-copy observations plus the existing positive control meet the agreed list without relabeling that tool result.
- (c) Descriptor stores: M697 deliberately measures small descriptor spans and detects twelve intentional frame uploads separately. The DWM050 store checkpoint counters reconcile; no pending spans. These metadata writes are not frame copies.
- (d) Final persistent-map census:24635 successful requests,24627 ended, eight live buffer maps, zero runtime-surface maps, no image maps crossing boundaries. The existing analyzer validates map/store sequencing and counts.

Together these satisfy the bounded no-copy element for this tested path. No new writer inventory is required absent contradictory data.

## VSync, TDR and boundaries

During the predeclared render interval12.659791..107.016846s,5656 VSyncDPCs have no missing reports, maximum gap17.621ms. The narrower KMD snapshot interval87.151s records5225ACKs and5225reports,59.953Hz, three DPC-recovered ACKs. Four170 skip-reason counters remain0. This run does not reproduce or repair the isolated DWM049 cadence defect.

Across the entire GPU adapter lifetime, all4307 MMIOFlips retire, no address mismatch, maximum latency16.838ms. Cropping at render-end leaves one final flip awaiting the next VSync; full-trace analysis proves it retired. Ten VSync gaps, maximum1.318s, occur during initial adapter/desktop transition before6.601s, outside the render interval. They are preserved, not labeled steady-state success or discarded as trace loss. Full trace has no lost events/buffers.

Start/end TDR summaries show zero recovery/debug collection, boot unchanged, immediate Display4101/WER117/141 query empty. WER can arrive later. The bounded run is not a long-duration stability claim.

## Scope of completion

This supplies the three G0 implementation evidence categories for the tested hosted Zink/RADV GPU desktop and native-window path, using the accepted bounded no-copy list. It does not close the separate longer M13.4 duration, M13.2 lifecycle coverage or M13.6/M13.7 fault/soak milestones. Owner limits every lab trial to180s. Deployment remains the verified CPU baseline after this trial; no permanent GPU promotion is implied.
