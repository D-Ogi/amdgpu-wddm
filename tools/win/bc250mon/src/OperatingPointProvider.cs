// The operating point the lab actually runs at: compute units, the DPM governor, the start's health and the
// GPU desktop interop switches. One panel, because an operator reads one glance, and because all four answer
// the same question: is this machine in the state the measurement assumes?
//
// The compute-unit row is the reason this panel exists (owner, 2026-10-05: "make sure our overlay shows how
// many CUs are really set"). The lab had run 24 CU instead of 40 since the move to tester release installs,
// because `CuMode` is in no INF and in no installer default table, and nothing said so.
//
// Cost. Two KMD escapes per poll, both adapter-owned software snapshots taken with NoAdapterSynchronization
// (BC250_ESCAPE_RUN_CU_MODE and BC250_ESCAPE_RUN_START_HEALTH): no BAR access, no SMU message, no scheduler
// idle, so neither takes the adapter lock a game's threads wait on (BD-054). Bc250CuMode uses the control
// DLL's cached adapter path, a few microseconds; Bc250StartHealth uses its uncached one, which walks SetupAPI
// for about 0.7 ms and is the path KmdProvider already takes. The DPM snapshot costs nothing at all, because
// TelemetryProvider reads it four times a second anyway and publishes it in a DpmFeed. The interop state comes
// from the driver's registry mirror, about ten microseconds, rather than from a third escape. The poll is every
// 10 seconds, and the pipeline panel's `graphics-summary.pause` marker stops the two escapes for a measured
// session: the rows then show the last snapshot with its time, so a paused panel never looks like a fresh one.
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

        /// <summary>The compute units in force now: the escape first, the driver's registry record after it.</summary>
        public uint? Applied
        {
            get
            {
                if (HaveEscape && Escape.Applied != 0) return Escape.Applied;
                if (HaveEscape && (Escape.Flags & CuModeSnapshot.FlagValid) != 0) return 0;   // the start knows: none
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
                if (HaveEscape) return Escape.Requested != 0 ? Escape.Requested : CuModeSnapshot.Stock;
                return Setting;
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
    /// The interop switches from the driver's registry mirror (driver/kmd/interop.c): `InteropLastState` is the
    /// effective bits plus the requested bits shifted left by 8, written at every full start. The mirror does
    /// not carry the escape's UNCLEAN, STALE or CLOSED_BY_DRIVER flags; "effective differs from requested"
    /// already names the state that matters, and `InteropSession` shows a marker nobody removed.
    /// </summary>
    public sealed class InteropView
    {
        public bool ParametersPresent;
        public int? LastState, Session, LastEnd;

        public int? Effective { get { return LastState.HasValue ? (int?)(LastState.Value & 0xFF) : null; } }
        public int? Requested { get { return LastState.HasValue ? (int?)((LastState.Value >> 8) & 0xFF) : null; } }
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
            var text = new StringBuilder();
            Level level;
            if (!applied.HasValue)
            {
                panel.Rows.Add(new Row("CU", "unknown: " + (v.EscapeError ?? "no driver record"), Level.Warn));
            }
            else
            {
                text.Append(applied.Value.ToString(CultureInfo.InvariantCulture)).Append(" CU");
                if (v.HaveEscape) text.Append(", ").Append(v.Escape.ActiveCus.ToString(CultureInfo.InvariantCulture)).Append(" counted");
                if ((v.Flags & CuModeSnapshot.FlagPending) != 0 && (v.Flags & CuModeSnapshot.FlagConfirmed) == 0)
                    text.Append(", PENDING: a restart falls back to 24");
                else if ((v.Flags & CuModeSnapshot.FlagConfirmed) != 0) text.Append(", confirmed");
                if (!v.HaveEscape) text.Append(" (driver record, no snapshot)");

                if (applied.Value == 0)
                    level = Level.Error;
                else if (v.Requested == CuModeSnapshot.Full && applied.Value == CuModeSnapshot.Stock)
                    level = Level.Error;                        // the 40 CU request fell back to the harvest
                else if (applied.Value != expected)
                    level = Level.Warn;
                else if ((v.Flags & CuModeSnapshot.FlagPending) != 0 && (v.Flags & CuModeSnapshot.FlagConfirmed) == 0)
                    level = Level.Warn;
                else if ((v.Flags & CuModeSnapshot.FlagValid) != 0 && (v.Flags & CuModeSnapshot.FlagConsistent) == 0)
                    level = Level.Warn;                         // CC and SPI name different WGPs
                else if (!v.HaveEscape)
                    level = Level.Info;                         // the record agrees, but nothing confirmed it now
                else
                    level = Level.Good;
                if (applied.Value != expected && applied.Value != 0)
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
                if (v.Confirmed.HasValue) value += ", confirmed";
                else if (v.Pending.HasValue) value += ", pending";
                panel.Rows.Add(new Row("CU setting", value,
                    setting.Value == expected ? Level.Good : Level.Warn));
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
            else if (s.Errors > 0) level = Level.Error;
            else if ((s.Flags & (DpmView.FlagPaused | DpmView.FlagStable)) != 0) level = Level.Warn;
            if (age > DpmView.StaleSeconds)
            {
                state.Add("snapshot " + age.ToString("0", CultureInfo.InvariantCulture) + " s old");
                if (level == Level.Good) level = Level.Warn;
            }
            panel.Rows.Add(new Row("DPM", string.Join(", ", state.ToArray()), level));

            var cap = new StringBuilder();
            cap.Append("cap ").Append(s.CapMHz.ToString(CultureInfo.InvariantCulture))
               .Append(" of ").Append(s.MaxMHz.ToString(CultureInfo.InvariantCulture)).Append(" MHz");
            cap.Append(", throttle ").Append(LabNames.DpmThrottleName(s.Throttle));
            if (s.Errors > 0) cap.Append(", ").Append(s.Errors.ToString(CultureInfo.InvariantCulture)).Append(" SMU errors");
            Level capLevel = Level.Good;
            if (s.Throttle == 6 || s.Errors > 0) capLevel = Level.Error;        // the governor stopped on the SMU
            else if (s.CapMHz < s.MaxMHz) capLevel = Level.Warn;                // a thermal cap binds the clock
            else if (s.Throttle == 1 || s.Throttle == 2 || s.Throttle == 8 || s.Throttle == 9) capLevel = Level.Warn;
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

            bool stalled = (s.Flags & StartHealthView.FlagFull) != 0 && s.LastCompletionAgeMs > StartHealthView.FreshMs;
            Level level;
            if (v.IdentityChanged) { level = Level.Error; text.Append(", the start changed in this session"); }
            else if ((s.Flags & StartHealthView.FlagFull) != 0 && (s.Flags & StartHealthView.FlagReady) == 0)
            { level = Level.Error; text.Append(", full table without ready"); }
            else if (stalled)
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
            if (!v.Effective.HasValue)
            {
                panel.Rows.Add(new Row("Interop", v.ParametersPresent ? "no InteropLastState record"
                                                                      : "no Parameters key", Level.Info));
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
            if (v.Session.HasValue)
            {
                text.Append("; session marked");
                if (level == Level.Good) level = Level.Warn;        // a marker nobody removed closes the next start
            }
            if (v.LastEnd.HasValue && v.LastEnd.Value == 1)
            {
                text.Append("; last end ").Append(LabNames.InteropEnd(v.LastEnd.Value));
                if (level == Level.Good) level = Level.Warn;
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
                  + (v.Health.Snapshot.LastCompletionAgeMs > StartHealthView.FreshMs ? " stalled" : "")));
            parts.Add("interop=" + (!v.Interop.Effective.HasValue ? "none"
                : v.Interop.Effective.Value.ToString(CultureInfo.InvariantCulture)
                  + "/" + v.Interop.Requested.Value.ToString(CultureInfo.InvariantCulture)
                  + (v.Interop.Session.HasValue ? " session" : "")
                  + " end=" + (v.Interop.LastEnd.HasValue ? v.Interop.LastEnd.Value.ToString(CultureInfo.InvariantCulture) : "-")));
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
