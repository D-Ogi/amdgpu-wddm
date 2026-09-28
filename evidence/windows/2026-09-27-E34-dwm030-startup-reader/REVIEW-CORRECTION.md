# Scope correction before commit

The live-writer positive control used FileShare.ReadWrite. It proves the reader
works with a cooperating writer, not with the current router's _wfreopen_s writable
stream. The CRT secure open denies sharing, so the reader correction alone does not
fix DWM030. A successor must change the router writer as well and test the actual CRT
opening modes, including the original deny-sharing mode as a negative control.
The host positive results remain valid only within that stated sharing model.
No successor may be treated as runtime-ready on these controls alone.
