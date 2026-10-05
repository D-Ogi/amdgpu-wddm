# E26 continuation through 0.7.54.1

PROVENANCE: Mesa 801c9763c6043f0de8408e905a5324eea06d81d7, MIT. The updated experiment patches include linear shared-resource import, OpenResource, DXGI Blt, and callback-backed Present. These are CPU diagnostic prototypes, not hardware D3D acceleration.

shared-0750 is the earlier text snapshot through KMD 0.7.50.1. aperture-0754 contains selected subsequent logs and the passing two-device shared-texture control. Raw ETL and BMP archives remain in workspace scratch/dwm. etw-allocation-diagnostics.txt extracts exact UTF-16 event payload strings without process/account metadata. The full trace is not committed.

Preserving reserved allocation flags in revision 53 did not solve sharing (the control entered with zero flags). ETW then identified the CPU-visible shared allocation segment restriction. Revision 54 selects aperture for the E26R shared-resource descriptor and sets normal allocation priority. Its signed SYS SHA256 is 2C281F1C10D2806E774C14D50317D8FF8DEC80D7D0F2194E171F7EAC459EA5EA.

The two-device red/blue test and 640x480 green staging readback pass. The probe completes its 60-Present loop with device status S_OK and task exit 0. Physical scanout remains black. DWM repeatedly creates and destroys its presentation resources; full desktop correctness remains open. The M8 hardware compute suite has not been rerun on these KMD revisions. All probes restore stub registration and display-only mode.
