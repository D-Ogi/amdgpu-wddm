// The operating point the lab actually runs at: compute units, the DPM governor, the start's health and the
// GPU desktop interop switches. One panel, because an operator reads one glance, and because all four answer
// the same question: is this machine in the state the measurement assumes?
//
// The compute-unit row is the reason this panel exists (owner, 2026-10-05: "make sure our overlay shows how
// many CUs are really set"). The lab had run 24 CU instead of 40 since the move to tester release installs,
// because `CuMode` is in no INF and in no installer default table, and nothing said so.
//
// Cost. Three KMD escapes per poll, all three adapter-owned software snapshots taken with
// NoAdapterSynchronization (BC250_ESCAPE_RUN_CU_MODE, BC250_ESCAPE_RUN_START_HEALTH, BC250_ESCAPE_RUN_INTEROP):
// no BAR access, no SMU message, no scheduler idle, so none takes the adapter lock a game's threads wait on
// (BD-054). Bc250CuMode and Bc250Interop use the control DLL's cached adapter path, a few microseconds each;
// Bc250StartHealth uses its uncached one, which walks SetupAPI for about 0.7 ms and is the path KmdProvider
// already takes. The DPM snapshot costs nothing at all, because TelemetryProvider reads it four times a second
// anyway and publishes it in a DpmFeed. The poll is every 10 seconds, and the pipeline panel's
// `graphics-summary.pause` marker stops all three escapes for a measured session: the rows then show the last
// snapshot with its time, so a paused panel never looks like a fresh one.
//
// The interop row needs the escape and not the registry mirror beside it, because the mirror cannot tell a live
// session from the trace of a machine that died with the GPU desktop path in use. That cost the first version of
// this panel an amber row on exactly the configuration the lab wants.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using Microsoft.Win32;

namespace Bc250Mon
{
    /// <summary>The CU mode registry record and, when it answered, the escape snapshot.</summary>
    public sealed class CuModeView
    {
        public bool ParametersPresent;              // the miniport's Parameters key exists
        public int? CuMode, CuDisableWgp, Pending, Confirmed, LastApplied, LastReason;
        public bool HaveEscape;
        public CuModeSnapshot Escape;
        public string EscapeError;                  // why the escape has no answer (an old control DLL says so)
        public DateTime EscapeUtc;                  // when the snapshot was taken (it survives a pause)

        /// <summary>
        /// The escape answered and the start ran the CU stage, so its numbers describe this hardware. Without
        /// `VALID` the start never reached the stage (a display-only start, or a full one that stopped at an
        /// earlier gate): `CuModeBegin` has zeroed the snapshot, and `Parameters\CuModeLastApplied` then still
        /// holds what the *previous* full start applied. Reporting that record as the state now would have shown
        /// a green "40 CU" over a part running the firmware's 24 CU harvest.
        /// </summary>
        public bool EscapeValid { get { return HaveEscape && (Escape.Flags & CuModeSnapshot.FlagValid) != 0; } }

        /// <summary>
        /// The compute units the registers name, counted as the caps are (`bc250_cu_info.active`, two per active
        /// WGP). This, not the mode, is the number a measurement depends on: mode 40 with `CuDisableWgp` naming
        /// one WGP runs 38 units. Null when no start answered for the hardware.
        /// </summary>
        public uint? Counted { get { return EscapeValid ? (uint?)Escape.ActiveCus : null; } }

        /// <summary>
        /// The mode in force: 24 or 40 from the start that ran the stage, else the driver's durable record of the
        /// last full start, and null when the escape answered without `VALID` (then nothing is known).
        /// </summary>
        public uint? Applied
        {
            get
            {
                if (EscapeValid) return Escape.Applied;
                if (HaveEscape) return null;
                return LastApplied.HasValue ? (uint?)(uint)LastApplied.Value : null;
            }
        }
        /// <summary>What the next start will apply: `CuMode`, and an absent value means the firmware's 24.</summary>
        public uint? Setting
        {
            get
            {
                if (CuMode.HasValue) return (uint)CuMode.Value;
                return ParametersPresent ? (uint?)CuModeSnapshot.Stock : null;
            }
        }
        /// <summary>What this start was asked for. The escape reports 0 for an absent setting, which means 24.</summary>
        public uint? Requested
        {
            get
            {
                if (EscapeValid) return Escape.Requested != 0 ? Escape.Requested : CuModeSnapshot.Stock;
                return Setting;
            }
        }

        /// <summary>
        /// `bc250_cu_encode(CuMode, CuDisableWgp)` of what `Parameters` asks for: the exact value `CuModePending`
        /// and `CuModeConfirmed` hold (bc250_cu_mode.h). Only a 40 request is encoded; `bc250_cu_decide` writes 0
        /// for 24 and clears a leftover mark. Null when the setting is not a 40 request.
        /// </summary>
        public int? Encoded
        {
            get
            {
                if (!CuMode.HasValue || CuMode.Value != (int)CuModeSnapshot.Full) return null;
                return (CuMode.Value & 0xFF) | ((CuDisableWgp.HasValue ? CuDisableWgp.Value : 0) << 8);
            }
        }
        /// <summary>
        /// The confirmation covers exactly this request. `bc250_cu_decide` marks a start pending unless
        /// `confirmed == encoded`, so a confirmation of plain 40 does not cover "40 with a WGP masked".
        /// </summary>
        public bool SettingConfirmed
        { get { return Encoded.HasValue && Confirmed.HasValue && Confirmed.Value == Encoded.Value; } }
        public bool SettingPending
        { get { return Encoded.HasValue && Pending.HasValue && Pending.Value == Encoded.Value; } }
        /// <summary>
        /// A 40 request whose mark on disk names a different request: the next start is PENDING all the same, and
        /// a crash in it falls back to 24. With a 24 request the marks mean nothing (the next start clears them),
        /// so this is false and the row says nothing about them.
        /// </summary>
        public bool SettingMarkedOther
        {
            get
            {
                if (!Encoded.HasValue || SettingConfirmed || SettingPending) return false;
                return Pending.HasValue || Confirmed.HasValue;
            }
        }
        public uint Flags { get { return HaveEscape ? Escape.Flags : 0u; } }
        public uint Reason
        {
            get
            {
                if (HaveEscape) return Escape.Reason;
                return LastReason.HasValue ? (uint)LastReason.Value : 0u;
            }
        }
    }

    /// <summary>The start-health snapshot plus the identity this session first saw.</summary>
    public sealed class StartHealthView
    {
        public bool Have;
        public StartHealthSnapshot Snapshot;
        public string Error;
        public DateTime Utc;
        public ulong FirstGeneration, FirstEpoch;   // 0 before the first answer

        public const uint FlagFull = 1, FlagReady = 2, FlagVisible = 4, FlagConfirmed = 8, FlagRequired = 7;
        public const ulong FreshMs = 15000;         // BC250_START_HEALTH_FRESH_MS
        /// <summary>
        /// `LastCompletionAgeMs` when nothing has completed yet: `HealthSnapshot` writes `~0ull` while
        /// `LastCompletion` is zero, and `HealthInvalidate` zeroes it at every visibility or mode change, at
        /// `StartHealthBegin` and at `StartHealthResumeReady` (driver/kmd/start_health.c). Printed as an age it
        /// read "18446744073709552 s ago", which is what a CPU desktop route showed permanently.
        /// </summary>
        public const ulong NoCompletion = ulong.MaxValue;

        /// <summary>No presentation has completed since the last invalidation. Not a stall: nothing started.</summary>
        public bool NothingCompleted { get { return Have && Snapshot.LastCompletionAgeMs == NoCompletion; } }
        /// <summary>A full start whose last completed presentation is older than the driver's own freshness bound.</summary>
        public bool Stalled
        {
            get
            {
                return Have && (Snapshot.Flags & FlagFull) != 0 && !NothingCompleted &&
                       Snapshot.LastCompletionAgeMs > FreshMs;
            }
        }

        /// <summary>The device start changed under a running session: every number taken before it is suspect.</summary>
        public bool IdentityChanged
        {
            get
            {
                return Have && FirstGeneration != 0 &&
                       (Snapshot.Generation != FirstGeneration || Snapshot.Epoch != FirstEpoch);
            }
        }
    }

    /// <summary>
    /// The GPU desktop interop switches of this start: `BC250_ESCAPE_RUN_INTEROP` op READ, with the driver's
    /// registry mirror (`InteropLastState` = effective bits | requested bits &lt;&lt; 8, written at every full
    /// start) as the fallback for a control DLL without the export.
    ///
    /// Why the escape and not the mirror alone. `InteropSession` is not a leftover marker: interop.c writes it at
    /// the first interop Blt present of a DDI device and deletes it when the last one goes, so on the lab's GPU
    /// desktop route it is on disk for the whole session - the healthy state. `InteropLastEnd` is written at every
    /// unmark and never cleared at a later start, so one driver swap leaves "device stop" there for the life of
    /// the machine. Only the start can tell a live marker from the trace of a machine that died with the path in
    /// use, and it says so in `FLAG_UNCLEAN` / `FLAG_STALE` / `FLAG_CLOSED_BY_DRIVER`, which the mirror has not got.
    /// </summary>
    public sealed class InteropView
    {
        public bool ParametersPresent;
        public int? LastState, Session, LastEnd;
        public bool HaveEscape;
        public InteropSnapshot Escape;
        public string EscapeError;                  // why the escape has no answer (an old control DLL says so)
        public DateTime EscapeUtc;

        /// <summary>The escape answered and a full WDDM start decided the switches.</summary>
        public bool EscapeValid { get { return HaveEscape && (Escape.Flags & InteropSnapshot.FlagValid) != 0; } }
        public uint EscapeFlags { get { return HaveEscape ? Escape.Flags : 0u; } }

        public int? Effective
        {
            get
            {
                if (EscapeValid) return (int)Escape.Effective;
                return LastState.HasValue ? (int?)(LastState.Value & 0xFF) : null;
            }
        }
        public int? Requested
        {
            get
            {
                if (EscapeValid) return (int)Escape.Requested;
                return LastState.HasValue ? (int?)((LastState.Value >> 8) & 0xFF) : null;
            }
        }
        public const int SwitchBlit = 1, SwitchCdd = 2, Both = 3;
    }

    public sealed class OperatingPointView
    {
        public CuModeView Cu = new CuModeView();
        public DpmView Dpm = new DpmView();
        public StartHealthView Health = new StartHealthView();
        public InteropView Interop = new InteropView();
        public bool Paused;                         // graphics-summary.pause: the escapes were not sent
        public DateTime NowUtc;
    }

    /// <summary>
    /// Pure rules: a view and the lab's expectations in, panel rows out. No registry, no escape, no clock, so
    /// `test-lab-state.ps1` checks every amber and every red rule on built views.
    /// </summary>
    public static class OperatingPointRules
    {
        /// <summary>
        /// Reasons that mean a 40 CU request did not hold in hardware, so the driver fell back and wrote
        /// `CuMode = 24` durably. The next start is 24 as well until somebody asks for 40 again.
        /// </summary>
        public static bool IsFallbackReason(uint reason)
        {
            return reason == 3 || reason == 6 || reason == 7 || reason == 8 || reason == 9;
        }

        public static void AddCuRows(Panel panel, CuModeView v, LabExpectations x)
        {
            uint expected = (uint)x.CuMode;
            uint? applied = v.Applied;
            uint? counted = v.Counted;
            var text = new StringBuilder();
            Level level;
            if (!applied.HasValue)
            {
                // Two different unknowns, and neither may read green. Without an escape there is no snapshot at
                // all; with one that has no VALID flag the start itself does not know, and the registry record of
                // an earlier full start describes hardware this start never touched.
                string why;
                if (v.HaveEscape)
                {
                    why = "unknown: this start did not run the CU stage";
                    if (v.LastApplied.HasValue)
                        why += " (an earlier start applied " + v.LastApplied.Value.ToString(CultureInfo.InvariantCulture) + ")";
                }
                else why = "unknown: " + (v.EscapeError ?? "no driver record");
                panel.Rows.Add(new Row("CU", why, Level.Warn));
            }
            else
            {
                // The number judged is the one the registers name, not the mode word: mode 40 with CuDisableWgp
                // naming one WGP applies 38 units, and 38 is what every measurement then runs on.
                uint judged = counted.HasValue ? counted.Value : applied.Value;
                text.Append(applied.Value.ToString(CultureInfo.InvariantCulture)).Append(" CU");
                if (counted.HasValue)
                    text.Append(", ").Append(counted.Value.ToString(CultureInfo.InvariantCulture)).Append(" counted");
                if ((v.Flags & CuModeSnapshot.FlagPending) != 0 && (v.Flags & CuModeSnapshot.FlagConfirmed) == 0)
                    text.Append(", PENDING: a restart falls back to 24");
                else if ((v.Flags & CuModeSnapshot.FlagConfirmed) != 0) text.Append(", confirmed");
                if (!v.HaveEscape) text.Append(" (driver record, no snapshot)");

                if (judged == 0 || applied.Value == 0)
                    level = Level.Error;
                else if (v.Requested == CuModeSnapshot.Full && applied.Value == CuModeSnapshot.Stock)
                    level = Level.Error;                        // the 40 CU request fell back to the harvest
                else if (judged != expected)
                    level = Level.Warn;
                else if ((v.Flags & CuModeSnapshot.FlagPending) != 0 && (v.Flags & CuModeSnapshot.FlagConfirmed) == 0)
                    level = Level.Warn;
                else if (v.EscapeValid && (v.Flags & CuModeSnapshot.FlagConsistent) == 0)
                    level = Level.Warn;                         // CC and SPI name different WGPs
                else if (!v.HaveEscape)
                    level = Level.Info;                         // the record agrees, but nothing confirmed it now
                else
                    level = Level.Good;
                if (judged != expected && judged != 0)
                    text.Append(", expected ").Append(expected.ToString(CultureInfo.InvariantCulture));
                panel.Rows.Add(new Row("CU", text.ToString(), level));
            }

            uint? setting = v.Setting;
            if (!setting.HasValue)
                panel.Rows.Add(new Row("CU setting", "no Parameters key", Level.Info));
            else
            {
                string value = v.CuMode.HasValue
                    ? "CuMode " + v.CuMode.Value.ToString(CultureInfo.InvariantCulture)
                    : "CuMode absent: the next start harvests to 24";
                if (v.CuDisableWgp.HasValue && v.CuDisableWgp.Value != 0)
                    value += " less WGP mask 0x" + v.CuDisableWgp.Value.ToString("X", CultureInfo.InvariantCulture);
                // The marks hold bc250_cu_encode(mode, disable), so "confirmed" is only true of this exact
                // request. A plain-40 confirmation does not cover 40 with a WGP masked, and the next start of
                // such a pair is PENDING: the row has to say so before the restart, not after it.
                bool otherMark = v.SettingMarkedOther;
                if (v.SettingConfirmed) value += ", confirmed";
                else if (v.SettingPending) value += ", pending";
                else if (otherMark) value += ", the mark on disk names another request: the next start is pending";
                else if (v.Encoded.HasValue) value += ", the next start marks it pending";
                // A mask holds the counted units below the mode, so a mode that equals the expectation still
                // cannot reach it: the next start applies fewer units than the measurement assumes.
                bool maskedBelow = v.CuDisableWgp.HasValue && v.CuDisableWgp.Value != 0;
                panel.Rows.Add(new Row("CU setting", value,
                    setting.Value == expected && !otherMark && !maskedBelow ? Level.Good : Level.Warn));
            }

            if (v.Reason != 0)
                panel.Rows.Add(new Row("CU reason", LabNames.CuReasonName(v.Reason),
                    IsFallbackReason(v.Reason) ? Level.Error : Level.Warn));
            if (v.HaveEscape || v.EscapeError == null || !applied.HasValue) return;
            panel.Rows.Add(new Row("CU snapshot", v.EscapeError, Level.Warn));
        }

        public static void AddDpmRows(Panel panel, DpmView v, DateTime nowUtc)
        {
            if (!v.Have)
            {
                panel.Rows.Add(new Row("DPM", v.Error ?? "no snapshot yet", Level.Warn));
                return;
            }
            DpmSnapshot s = v.Snapshot;
            double age = (nowUtc - v.Utc).TotalSeconds;
            var state = new List<string>();
            state.Add(s.Mode == DpmView.ModeDpm ? "dpm" : "fixed-lab");
            if ((s.Flags & DpmView.FlagGoverning) != 0) state.Add("governing");
            else if ((s.Flags & DpmView.FlagRunning) != 0) state.Add("NOT governing");
            else state.Add("governor stopped");
            if ((s.Flags & DpmView.FlagPending) != 0 && (s.Flags & DpmView.FlagConfirmed) == 0)
                state.Add("PENDING: a restart falls back to fixed-lab");
            else if ((s.Flags & DpmView.FlagConfirmed) != 0) state.Add("confirmed");
            if ((s.Flags & DpmView.FlagPaused) != 0) state.Add("paused");
            if ((s.Flags & DpmView.FlagStable) != 0) state.Add("SetStablePowerState: pinned to the floor");

            Level level = Level.Good;
            if ((s.Flags & DpmView.FlagPending) != 0 && (s.Flags & DpmView.FlagConfirmed) == 0) level = Level.Error;
            else if (s.Mode == DpmView.ModeDpm && (s.Flags & DpmView.FlagGoverning) == 0) level = Level.Error;
            else if ((s.Flags & (DpmView.FlagPaused | DpmView.FlagStable)) != 0) level = Level.Warn;
            if (age > DpmView.StaleSeconds)
            {
                state.Add("snapshot " + age.ToString("0", CultureInfo.InvariantCulture) + " s old");
                if (level == Level.Good) level = Level.Warn;
            }
            // A failure after a good snapshot leaves the snapshot in the feed and the reason beside it. Without
            // this line a driver that stopped answering reported an age and never why.
            if (v.Error != null)
            {
                state.Add(v.Error);
                if (level == Level.Good) level = Level.Warn;
            }
            panel.Rows.Add(new Row("DPM", string.Join(", ", state.ToArray()), level));

            var cap = new StringBuilder();
            cap.Append("cap ").Append(s.CapMHz.ToString(CultureInfo.InvariantCulture))
               .Append(" of ").Append(s.MaxMHz.ToString(CultureInfo.InvariantCulture)).Append(" MHz");
            cap.Append(", throttle ").Append(LabNames.DpmThrottleName(s.Throttle));
            // Errors is the cumulative count of failed SMU transitions of this start (dpm.c). The governor gives
            // up only after BC250_DPM_ERROR_LIMIT = 3 failures in a row and resets the counter on every success,
            // so one recovered retry during a game is a statistic, not a stopped governor. Giving up is already
            // red twice over: mode DPM without GOVERNING, and throttle 6 (SMU).
            if (s.Errors > 0) cap.Append(", ").Append(s.Errors.ToString(CultureInfo.InvariantCulture)).Append(" SMU errors");
            Level capLevel = Level.Good;
            if (s.Throttle == 6) capLevel = Level.Error;                        // the governor stopped on the SMU
            else if (s.CapMHz < s.MaxMHz) capLevel = Level.Warn;                // a thermal cap binds the clock
            else if (s.Throttle == 1 || s.Throttle == 2 || s.Throttle == 8 || s.Throttle == 9) capLevel = Level.Warn;
            else if (s.Errors > 0) capLevel = Level.Warn;
            panel.Rows.Add(new Row("DPM cap", cap.ToString(), capLevel));
        }

        public static void AddStartHealthRows(Panel panel, StartHealthView v)
        {
            if (!v.Have)
            {
                panel.Rows.Add(new Row("Start health", v.Error ?? "no snapshot yet", Level.Warn));
                return;
            }
            StartHealthSnapshot s = v.Snapshot;
            var names = new List<string>();
            if ((s.Flags & StartHealthView.FlagFull) != 0) names.Add("full");
            if ((s.Flags & StartHealthView.FlagReady) != 0) names.Add("ready");
            if ((s.Flags & StartHealthView.FlagVisible) != 0) names.Add("visible");
            if ((s.Flags & StartHealthView.FlagConfirmed) != 0) names.Add("confirmed");
            var text = new StringBuilder();
            text.Append("flags ").Append(s.Flags.ToString(CultureInfo.InvariantCulture))
                .Append(names.Count > 0 ? " (" + string.Join(" ", names.ToArray()) + ")" : " (none)");
            text.Append(", gen ").Append(s.Generation.ToString(CultureInfo.InvariantCulture))
                .Append(" epoch ").Append(s.Epoch.ToString(CultureInfo.InvariantCulture));

            // "Nothing has completed yet" is not a stall and must not be printed as an age: the driver writes
            // ~0ull for it, and the CPU desktop route, which completes no GPU presentation at all, showed
            // "18446744073709552 s ago" for ever.
            if (v.NothingCompleted) text.Append(", no completed presentation yet");
            Level level;
            if (v.IdentityChanged) { level = Level.Error; text.Append(", the start changed in this session"); }
            else if ((s.Flags & StartHealthView.FlagFull) != 0 && (s.Flags & StartHealthView.FlagReady) == 0)
            { level = Level.Error; text.Append(", full table without ready"); }
            else if (v.Stalled)
            {
                level = Level.Warn;
                text.Append(", last completion ")
                    .Append((s.LastCompletionAgeMs / 1000.0).ToString("0", CultureInfo.InvariantCulture)).Append(" s ago");
            }
            else if ((s.Flags & StartHealthView.FlagConfirmed) == 0) level = Level.Warn;
            else level = Level.Good;
            panel.Rows.Add(new Row("Start health", text.ToString(), level));
        }

        public static void AddInteropRows(Panel panel, InteropView v)
        {
            // The escape answered, but no full WDDM start decided the switches (a display-only start, or one that
            // stopped earlier): Requested and Effective are 0 and mean nothing. Say that instead of reading the
            // zeros as a closed desktop path.
            if (v.HaveEscape && !v.EscapeValid)
            {
                panel.Rows.Add(new Row("Interop", "no full WDDM start decided the switches ("
                    + LabNames.InteropReasonName(v.Escape.Reason) + ")", Level.Warn));
                return;
            }
            if (!v.Effective.HasValue)
            {
                panel.Rows.Add(new Row("Interop", v.EscapeError ?? (v.ParametersPresent
                    ? "no InteropLastState record" : "no Parameters key"), Level.Info));
                return;
            }
            int effective = v.Effective.Value, requested = v.Requested.Value;
            var text = new StringBuilder();
            text.Append("effective ").Append(LabNames.InteropSwitches(effective));
            Level level;
            if (effective != requested)
            {
                text.Append(", requested ").Append(LabNames.InteropSwitches(requested))
                    .Append(": the driver closed the GPU desktop path");
                level = Level.Error;
            }
            else if (effective != InteropView.Both)
            {
                text.Append(": the GPU desktop path is not complete");
                level = Level.Error;
            }
            else { text.Append(", as requested"); level = Level.Good; }
            if (v.HaveEscape)
            {
                uint flags = v.Escape.Flags;
                if ((flags & InteropSnapshot.FlagUnclean) != 0)
                {
                    text.Append("; this start found a marker of an earlier boot: the last one died with the path in use");
                    level = Level.Error;
                }
                else if ((flags & InteropSnapshot.FlagStale) != 0)
                {
                    text.Append("; this start found a marker of this boot (a start that ended without an unmark)");
                    if (level == Level.Good) level = Level.Warn;
                }
                if ((flags & InteropSnapshot.FlagClosedByDriver) != 0)
                {
                    text.Append("; closed by the driver (")
                        .Append(LabNames.InteropReasonName(v.Escape.ClosedReason)).Append(")");
                    level = Level.Error;
                }
                if ((flags & InteropSnapshot.FlagPersistFailed) != 0)
                {
                    text.Append("; the durable close failed: the next start sees the same marker");
                    level = Level.Error;
                }
                if ((flags & InteropSnapshot.FlagDown) != 0)
                {
                    text.Append("; a system power transition began");
                    if (level == Level.Good) level = Level.Warn;
                }
                // Neutral, never a level: a marked session with devices using the path is what a working GPU
                // desktop looks like. The flags above are the ones that mean something went wrong.
                if ((flags & InteropSnapshot.FlagSession) != 0)
                    text.Append("; session live, ").Append(v.Escape.Users.ToString(CultureInfo.InvariantCulture))
                        .Append(v.Escape.Users == 1 ? " device" : " devices");
            }
            else
            {
                // The mirror alone: both of these are normal states, so neither changes the level. `InteropSession`
                // is on disk while a device that used the path is alive, and `InteropLastEnd` keeps the last
                // unmark of any boot for ever.
                if (v.Session.HasValue) text.Append("; session marked");
                if (v.LastEnd.HasValue && v.LastEnd.Value != 0)
                    text.Append("; previous end ").Append(LabNames.InteropEnd(v.LastEnd.Value));
                text.Append(" (registry mirror, no snapshot)");
            }
            panel.Rows.Add(new Row("Interop", text.ToString(), level));
        }

        public static List<Row> Rows(OperatingPointView v, LabExpectations x)
        {
            var panel = new Panel();
            AddCuRows(panel, v.Cu, x);
            AddDpmRows(panel, v.Dpm, v.NowUtc);
            AddStartHealthRows(panel, v.Health);
            AddInteropRows(panel, v.Interop);
            return panel.Rows;
        }

        /// <summary>
        /// What a state change is, for the log. Deliberately coarse: it carries no clock, no temperature and no
        /// throttle, because those change in every game and would fill the log with normal thermal behaviour.
        /// </summary>
        public static string StateKey(OperatingPointView v, LabExpectations x)
        {
            var parts = new List<string>();
            parts.Add("cu=" + (v.Cu.Applied.HasValue ? v.Cu.Applied.Value.ToString(CultureInfo.InvariantCulture) : "?")
                      + "/" + x.CuMode.ToString(CultureInfo.InvariantCulture)
                      + " counted=" + (v.Cu.Counted.HasValue ? v.Cu.Counted.Value.ToString(CultureInfo.InvariantCulture) : "?")
                      + " set=" + (v.Cu.Setting.HasValue ? v.Cu.Setting.Value.ToString(CultureInfo.InvariantCulture) : "?")
                      + " flags=" + v.Cu.Flags.ToString(CultureInfo.InvariantCulture)
                      + " reason=" + v.Cu.Reason.ToString(CultureInfo.InvariantCulture)
                      + " snapshot=" + (v.Cu.HaveEscape ? "yes" : "no"));
            parts.Add("dpm=" + (!v.Dpm.Have ? "none"
                : v.Dpm.Snapshot.Mode.ToString(CultureInfo.InvariantCulture)
                  + "/" + (v.Dpm.Snapshot.Flags & (DpmView.FlagRunning | DpmView.FlagGoverning | DpmView.FlagPending
                                                   | DpmView.FlagConfirmed | DpmView.FlagPaused | DpmView.FlagStable))
                            .ToString(CultureInfo.InvariantCulture)
                  + " max=" + v.Dpm.Snapshot.MaxMHz.ToString(CultureInfo.InvariantCulture)
                  + " errors=" + (v.Dpm.Snapshot.Errors > 0 ? "yes" : "no")));
            parts.Add("health=" + (!v.Health.Have ? "none"
                : v.Health.Snapshot.Flags.ToString(CultureInfo.InvariantCulture)
                  + " gen=" + v.Health.Snapshot.Generation.ToString(CultureInfo.InvariantCulture)
                  + " epoch=" + v.Health.Snapshot.Epoch.ToString(CultureInfo.InvariantCulture)
                  + (v.Health.NothingCompleted ? " nothing-completed" : v.Health.Stalled ? " stalled" : "")));
            // The interop flags in the key are the start-latched ones only. SESSION and DOWN are left out: a
            // device beginning or ending its use of the path is normal traffic, not a change of state.
            parts.Add("interop=" + (!v.Interop.Effective.HasValue
                ? (v.Interop.HaveEscape ? "not-decided" : "none")
                : v.Interop.Effective.Value.ToString(CultureInfo.InvariantCulture)
                  + "/" + v.Interop.Requested.Value.ToString(CultureInfo.InvariantCulture)
                  + (v.Interop.HaveEscape
                     ? " flags=" + (v.Interop.EscapeFlags & (InteropSnapshot.FlagValid | InteropSnapshot.FlagUnclean
                                                             | InteropSnapshot.FlagStale | InteropSnapshot.FlagClosedByDriver
                                                             | InteropSnapshot.FlagPersistFailed))
                                     .ToString(CultureInfo.InvariantCulture)
                     : " mirror")));
            return string.Join("; ", parts.ToArray());
        }

        /// <summary>The worst level among the rows: what the log line is filed as.</summary>
        public static Level Worst(IEnumerable<Row> rows)
        {
            Level worst = Level.Good;
            foreach (var row in rows) if (row.Level > worst) worst = row.Level;
            return worst;
        }
    }

    public sealed class OperatingPointProvider : IProvider
    {
        public const string ParametersKey = @"SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters";

        readonly ILabStateSource _source;
        readonly DpmFeed _dpm;
        readonly ExpectationsFile _expectations;
        readonly string _pausePath;
        CuModeSnapshot _cu;
        bool _haveCu;
        string _cuError;
        DateTime _cuUtc;
        StartHealthSnapshot _health;
        bool _haveHealth;
        string _healthError;
        DateTime _healthUtc;
        InteropSnapshot _interop;
        bool _haveInterop;
        string _interopError;
        DateTime _interopUtc;
        ulong _firstGeneration, _firstEpoch;
        string _lastKey;

        public OperatingPointProvider(ILabStateSource source, DpmFeed dpm, string dataDir)
        {
            _source = source;
            _dpm = dpm;
            _expectations = new ExpectationsFile(dataDir);
            _pausePath = System.IO.Path.Combine(dataDir ?? ".", GraphicsPipelineProvider.SummaryPauseFileName);
        }

        public string Name { get { return "operating"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(10); } }

        public void Poll(State state)
        {
            var panel = new Panel { Name = Name, Title = "Operating point", Order = 14 };
            LabExpectations expectations = _expectations.Read();
            bool paused;
            try { paused = File.Exists(_pausePath); } catch { paused = false; }
            if (!paused) ReadEscapes();

            var view = new OperatingPointView { Paused = paused, NowUtc = DateTime.UtcNow };
            RegistryDwords parameters = RegistryDwords.Read(Registry.LocalMachine, ParametersKey);
            view.Cu = BuildCu(parameters);
            view.Dpm = _dpm != null ? _dpm.Read() : new DpmView { Error = "no telemetry feed" };
            view.Health = new StartHealthView
            {
                Have = _haveHealth, Snapshot = _health, Error = _healthError, Utc = _healthUtc,
                FirstGeneration = _firstGeneration, FirstEpoch = _firstEpoch,
            };
            view.Interop = new InteropView
            {
                ParametersPresent = parameters.Present,
                LastState = parameters.Get("InteropLastState"),
                Session = parameters.Get("InteropSession"),
                LastEnd = parameters.Get("InteropLastEnd"),
                HaveEscape = _haveInterop, Escape = _interop, EscapeError = _interopError, EscapeUtc = _interopUtc,
            };

            List<Row> rows = OperatingPointRules.Rows(view, expectations);
            foreach (var row in rows) panel.Rows.Add(row);
            if (expectations.Error != null)
                panel.Rows.Add(new Row("Expectations", _expectations.FilePath + ": " + expectations.Error, Level.Warn));
            panel.Rows.Add(new Row("Sampled", paused
                ? "paused; snapshot " + (_cuUtc == default(DateTime) ? "none"
                                         : _cuUtc.ToLocalTime().ToString("HH:mm:ss"))
                : DateTime.Now.ToString("HH:mm:ss") + " (10 s poll)", paused ? Level.Warn : Level.Info));
            state.SetPanel(panel);

            string key = OperatingPointRules.StateKey(view, expectations);
            if (key == _lastKey) return;
            _lastKey = key;
            state.Log(Name, OperatingPointRules.Worst(rows), key);
        }

        // An old bc250control.dll has no Bc250CuMode export: the first call throws EntryPointNotFoundException
        // and every later one does the same. The row then says so and the rest of the panel keeps working.
        void ReadEscapes()
        {
            try
            {
                _cu = _source.ReadCuMode();
                _haveCu = true;
                _cuError = null;
                _cuUtc = DateTime.UtcNow;
            }
            catch (EntryPointNotFoundException)
            {
                _haveCu = false;
                _cuError = "control DLL too old for CU mode";
            }
            catch (Exception e)
            {
                _haveCu = false;
                _cuError = e.Message;
            }
            try
            {
                _health = _source.ReadStartHealthSnapshot();
                _haveHealth = true;
                _healthError = null;
                _healthUtc = DateTime.UtcNow;
                if (_firstGeneration == 0 && _health.Generation != 0)
                { _firstGeneration = _health.Generation; _firstEpoch = _health.Epoch; }
            }
            catch (EntryPointNotFoundException)
            {
                _haveHealth = false;
                _healthError = "control DLL too old for start health";
            }
            catch (Exception e)
            {
                _haveHealth = false;
                _healthError = e.Message;
            }
            try
            {
                _interop = _source.ReadInterop();
                _haveInterop = true;
                _interopError = null;
                _interopUtc = DateTime.UtcNow;
            }
            catch (EntryPointNotFoundException)
            {
                _haveInterop = false;
                _interopError = "control DLL too old for the interop snapshot";
            }
            catch (Exception e)
            {
                _haveInterop = false;
                _interopError = e.Message;
            }
        }

        CuModeView BuildCu(RegistryDwords parameters)
        {
            return new CuModeView
            {
                ParametersPresent = parameters.Present,
                CuMode = parameters.Get("CuMode"),
                CuDisableWgp = parameters.Get("CuDisableWgp"),
                Pending = parameters.Get("CuModePending"),
                Confirmed = parameters.Get("CuModeConfirmed"),
                LastApplied = parameters.Get("CuModeLastApplied"),
                LastReason = parameters.Get("CuModeLastReason"),
                HaveEscape = _haveCu,
                Escape = _cu,
                EscapeError = _cuError,
                EscapeUtc = _cuUtc,
            };
        }
    }
}
