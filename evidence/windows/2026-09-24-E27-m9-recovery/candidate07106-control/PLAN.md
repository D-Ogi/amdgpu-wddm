# M342 - First-start RLC control after AC recovery

Compare the same106 before/afterPSP observations with M341 warm failure, using the recovered boot04:16:46. Verify exact installed SYS, healthy closed-gate adapter, STOP,1000MHz/820mV and temperature<85C. One full start with the existing106 script, no OS reboot. Preserve stream/process handle. Success requires scheduler-read/write/done, healthy adapter and full-start completion; raw RLC values alone are not success. If failed, recover using existing authorized procedure without blind rerun. This controls boot history, not all timing or prior firmware state. No automatic reset implementation is introduced.

After a successful first start, run the unchanged64KiB M337 probe as a short actualGPU workload, then close all execution gates and perform one PnP device restart into display-only. This executes106after-stop observation without another fullGPU reinitialization. Capture persisted stop and the current diagnostic state before any further experiment.

After observing after-stop busyclear, enable only the MMIO read gate in display-only mode and restart the device once with every execution/write gate closed. Read RLC_CNTL/GRBM_STATUS2 using regcalc offsets and the existing CLI to observe state after PSP unload/GART restoration. This mapping-only transition does not initialize GPU engines. Keep full gate closed.
