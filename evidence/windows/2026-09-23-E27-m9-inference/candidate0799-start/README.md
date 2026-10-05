# M267: candidate0799 installation and unresolved full startup

2026-09-23, unit A. New encoding diagnostics count valid system leaf encoding attempts by OS CacheCoherent and record disagreement with AMD SNOOPED. Counts include repeated encoding, not GPU completions. Host controls verify coherent/noncoherent encoding.13887 routing checks pass. WDK build/sign passes, SYS SHA2566B271C001A0747EE02142AF2E8C3432BD0E8B2056A2678EBFAB3C77916ABE8B9; candidate0.7.99.1 package-umd.

Installation with hardware gates closed succeeds: pnputil0,oem73.inf, deviceOK, installed version/hash match, gates remain closed, DWM1. Boot time remains2026-09-23T13:18:44. No Windows reboot was requested.

Subsequent one-shot full WDDM attempt verifies firmware,1000MHz/VID116 and temperature before PnP disable15:17:38/enable15:17:42. Both PnP commands report success. Output then stops before state/driver log capture; independent SSH command reaches15-second timeout. Startup outcome and GPU failure stage are unknown. No new ICD or GPU application test was run. start-partial.log is an immutable snapshot of an unfinished stream, not a completed-run log. Owner screen observation was requested with no reset yet. Adapter instance suffix redacted from captured logs; raw streams retained outside repo.

Do not attribute this to cache intent or infer a successful full-WDDM start. Reinitialization is a preexisting open requirement. Recover logs/state before another attempt. Full goal remains active.
