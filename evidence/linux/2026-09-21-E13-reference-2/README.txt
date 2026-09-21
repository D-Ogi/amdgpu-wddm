E13: second Linux reference session on unit A, 2026-09-21. Procedure and result: experiments/E13-linux-reference-2/README.md.

Alpine diagnostic stick, kernel 6.18.52-0-lts, amdgpu as shipped with it. Captured by experiments/E13-linux-reference-2/
session.sh over SSH, copied here through redact.py (MAC addresses and USB serial numbers replaced, the stick's network
log left out). Nothing here was edited by hand.

boot1-full/        started by the owner out of Windows (cold or warm not recorded), stick mode "full". state.txt and ib.txt
                   are the PC-side copies of the phase output. This boot ended in a hang at "modprobe -r amdgpu"; what that
                   step had written to RAM is lost.
boot2-full-cold/   cold start by the owner's power button, mode "full".
boot3-readonly/    warm restart out of boot 2, mode "readonly" (amdgpu not loaded by the stick), then "load" with tracefs
                   armed: amdgpu-events-load.txt is the clean init trace (87778 events, no overrun).
compare-e03.txt    compare.py: E03's trace against boot 3's.

Per boot: pre.txt (PCI view, interrupts), state.txt (dmesg count, module parameters, MSI state, firmware info and file
hashes, memory managers, pm info, gpu_metrics, DTN log, named registers through amdgpu_regs2 incl. the per-queue HQD
registers under amdgpu's own SRBM selection), ib.txt + amdgpu-events-ib.txt (amdgpu_test_ib under the event trace),
rings/ and rings-after-ib/ (ring buffers and MQDs as 32-bit words), dmesg.txt.
File times inside the captures are two hours ahead: the stick reads the RTC as UTC.
