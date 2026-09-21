// bc250kmd's trail in the registry, on the overlay. The lab has no kernel debugger (ADR 0006), so the
// miniport leaves everything it can say about itself under
//
//   HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters
//     UnconfirmedStarts  REG_DWORD  raised by the driver at every start, cleared from here once the desktop is up
//     LastStage          REG_DWORD  BC250_STAGE
//     StageHistory       REG_SZ     the stages of this boot, oldest first
//
// This file holds the reader (KmdRegistry), the stage names (KmdStages) and the panel (KmdProvider). The
// confirmation of ADR 0006 point 3 lives here too: this process runs in the interactive session, so the fact
// that it is polling at all is the evidence that the desktop came up.
using System;
using System.Globalization;
using System.IO;
using Microsoft.Win32;

namespace Bc250Mon
{
    public sealed class KmdStage
    {
        public readonly int Number;
        public readonly string Symbol, Text;        // Symbol is the C enumerator, Text what the overlay shows
        public KmdStage(int number, string symbol, string text) { Number = number; Symbol = symbol; Text = text; }
    }

    public static class KmdStages
    {
        // MUST STAY IN SYNC with enum BC250_STAGE in driver/kmd/bc250kmd.h (numbers and enumerator names).
        // test_stages.py parses that header and this table and fails on any difference; build.ps1 runs it.
        public static readonly KmdStage[] All =
        {
            new KmdStage(0, "StageNone", "nothing written yet"),
            new KmdStage(10, "StageDriverEntry", "DriverEntry"),
            new KmdStage(20, "StageAddDevice", "add device"),
            new KmdStage(30, "StageStartEnter", "start: entered"),
            new KmdStage(31, "StageStartGuardPassed", "start: guard passed"),
            new KmdStage(32, "StageStartDeviceInfo", "start: device info"),
            new KmdStage(33, "StageStartPostDisplayAcquired", "start: post display acquired"),
            new KmdStage(34, "StageStartFramebufferMapped", "start: framebuffer mapped"),
            new KmdStage(35, "StageStartMmioDone", "start: register gate handled"),
            new KmdStage(39, "StageStartDone", "start: done"),
            new KmdStage(50, "StageFirstCommitVidPn", "first CommitVidPn"),
            new KmdStage(60, "StageFirstPresent", "first present"),
            new KmdStage(61, "StageFirstPresentDone", "first present done"),
            new KmdStage(70, "StageStopEnter", "stop: entered"),
            new KmdStage(79, "StageStopDone", "stop: done"),
            new KmdStage(90, "StageRefusedByGuard", "refused by the guard"),
            new KmdStage(91, "StageStartFailed", "start failed"),
        };

        // Also mirrored from driver/kmd/bc250kmd.h, and checked by test_stages.py.
        public const int FirstPresentDone = 61;             // StageFirstPresentDone: from here a start is trustworthy
        public const int MaxUnconfirmedStarts = 2;          // BC250_MAX_UNCONFIRMED_STARTS: the driver refuses to start

        public static string Name(int number)
        {
            foreach (var s in All) if (s.Number == number) return s.Text;
            return "unknown stage";
        }

        public static string Describe(int number)
        {
            return number + " " + Name(number);
        }

        /// <summary>The end of StageHistory, which is where the interesting part is after a hang.</summary>
        public static string Tail(string history, int items)
        {
            if (string.IsNullOrEmpty(history)) return "-";
            string[] all = history.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            int skip = Math.Max(0, all.Length - items);
            string tail = string.Join(" ", all, skip, all.Length - skip);
            return (skip > 0 ? "... " : "") + tail;
        }
    }

    public sealed class KmdSnapshot
    {
        public bool Installed;                  // the service key exists
        public bool Started;                    // the Parameters key exists, so the driver has run at least once
        public int? LastStage, UnconfirmedStarts;
        public string History;
        public string Error;                    // the key is there but could not be read
    }

    /// <summary>
    /// Reader and writer of the miniport's registry key. One instance is shared by the provider and the
    /// actions, so both look at the same place, including when a test key is used instead of the service.
    /// </summary>
    public sealed class KmdRegistry
    {
        public const string DefaultPath = @"HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd";
        readonly RegistryKey _hive;
        readonly string _subPath;
        public readonly string Path;            // what the panel and the log show

        /// <param name="overridePath">
        /// Debug switch (env BC250MON_KMD_KEY): another key to read and write, e.g.
        /// <c>HKCU\Software\BC250\kmdtest</c>. It exists so that the "installed" paths of this code can be
        /// exercised on a machine where no such service is installed and without administrator rights.
        /// </param>
        public KmdRegistry(string overridePath)
        {
            Path = string.IsNullOrEmpty(overridePath) ? DefaultPath : overridePath.Trim();
            string rest;
            _hive = SplitHive(Path, out rest);
            _subPath = rest;
        }

        static RegistryKey SplitHive(string path, out string rest)
        {
            int slash = path.IndexOf('\\');
            string hive = slash < 0 ? path : path.Substring(0, slash);
            rest = slash < 0 ? "" : path.Substring(slash + 1);
            switch (hive.ToUpperInvariant())
            {
                case "HKCU":
                case "HKEY_CURRENT_USER": return Registry.CurrentUser;
                case "HKLM":
                case "HKEY_LOCAL_MACHINE": return Registry.LocalMachine;
                default: throw new ArgumentException("key must start with HKLM or HKCU: " + path);
            }
        }

        static int? AsInt(object value)
        {
            return value is int ? (int?)(int)value : null;
        }

        public KmdSnapshot Read()
        {
            var s = new KmdSnapshot();
            try
            {
                using (var service = _hive.OpenSubKey(_subPath, false))
                {
                    if (service == null) return s;           // not installed: the normal case today, not an error
                    s.Installed = true;
                    using (var parameters = service.OpenSubKey("Parameters", false))
                    {
                        if (parameters == null) return s;    // installed but never started
                        s.Started = true;
                        s.LastStage = AsInt(parameters.GetValue("LastStage"));
                        s.UnconfirmedStarts = AsInt(parameters.GetValue("UnconfirmedStarts"));
                        s.History = parameters.GetValue("StageHistory") as string;
                    }
                }
            }
            catch (Exception e) { s.Error = e.Message; }
            return s;
        }

        /// <summary>Tell the driver that a human saw the desktop: UnconfirmedStarts = 0. Throws if it cannot.</summary>
        public void Confirm()
        {
            using (var service = _hive.OpenSubKey(_subPath, true))
            {
                if (service == null) throw new InvalidOperationException("bc250kmd is not installed (" + Path + ")");
                using (var parameters = service.CreateSubKey("Parameters"))
                    parameters.SetValue("UnconfirmedStarts", 0, RegistryValueKind.DWord);
            }
        }

        public string Summary()
        {
            var s = Read();
            if (!s.Installed) return "bc250kmd not installed (" + Path + ")";
            if (s.Error != null) return "bc250kmd key unreadable: " + s.Error;
            if (!s.Started) return "bc250kmd installed, never started (no Parameters key)";
            return string.Format(CultureInfo.InvariantCulture, "bc250kmd UnconfirmedStarts={0} of {1}, LastStage={2}, history {3}",
                s.UnconfirmedStarts.HasValue ? s.UnconfirmedStarts.Value.ToString(CultureInfo.InvariantCulture) : "-",
                KmdStages.MaxUnconfirmedStarts,
                s.LastStage.HasValue ? KmdStages.Describe(s.LastStage.Value) : "-",
                KmdStages.Tail(s.History, 12));
        }
    }

    public sealed class KmdProvider : IProvider
    {
        // ADR 0006 point 3: a start counts as good once the desktop has been up for this long. This process is
        // started at logon in the interactive session, so its own age is the desktop's age.
        public const int ConfirmAfterSeconds = 60;

        readonly KmdRegistry _kmd;
        readonly int _startTick = Environment.TickCount;
        bool? _wasInstalled;
        string _lastError;

        public KmdProvider(KmdRegistry kmd, string dataDir)
        {
            _kmd = kmd;
        }

        public string Name { get { return "kmd"; } }
        public TimeSpan Period { get { return TimeSpan.FromSeconds(5); } }

        int UptimeSeconds { get { return unchecked(Environment.TickCount - _startTick) / 1000; } }

        // The overlay is 460 px wide and the service path wraps over three lines there, so it is only worth a
        // row when it is not the usual one, i.e. when the debug switch points this provider somewhere else.
        void AddKeyRow(Panel p)
        {
            if (_kmd.Path != KmdRegistry.DefaultPath) p.Rows.Add(new Row("Key", _kmd.Path, Level.Warn));
        }

        public void Poll(State state)
        {
            var p = new Panel { Name = Name, Title = "bc250kmd", Order = 30 };
            var s = _kmd.Read();

            if (!s.Installed)
            {
                // The normal state of the lab today. Say it once in the log, then stay quiet.
                p.Rows.Add(new Row("Driver", "not installed"));
                AddKeyRow(p);
                if (_wasInstalled == true) state.Log(Name, Level.Info, "the bc250kmd service key is gone");
                _wasInstalled = false;
                _lastError = null;
                state.SetPanel(p);
                return;
            }
            if (_wasInstalled == false) state.Log(Name, Level.Good, "the bc250kmd service key appeared");
            _wasInstalled = true;

            p.Rows.Add(new Row("Driver", "installed", Level.Good));
            AddKeyRow(p);
            if (s.Error != null)
            {
                p.Rows.Add(new Row("Key", s.Error, Level.Error));
                if (_lastError != s.Error) state.Log(Name, Level.Error, "cannot read " + _kmd.Path + ": " + s.Error);
                _lastError = s.Error;
                state.SetPanel(p);
                return;
            }
            _lastError = null;

            if (!s.Started)
            {
                p.Rows.Add(new Row("Stage", "never started (no Parameters key)"));
                state.SetPanel(p);
                return;
            }

            p.Rows.Add(new Row("Stage", s.LastStage.HasValue ? KmdStages.Describe(s.LastStage.Value) : "-",
                               s.LastStage.HasValue && (s.LastStage.Value == 90 || s.LastStage.Value == 91) ? Level.Error : Level.Info));
            p.Rows.Add(new Row("History", KmdStages.Tail(s.History, 12)));

            int starts = s.UnconfirmedStarts ?? 0;
            Level level = starts >= KmdStages.MaxUnconfirmedStarts ? Level.Error : starts >= 1 ? Level.Warn : Level.Good;
            p.Rows.Add(new Row("Unconfirmed", starts + " of " + KmdStages.MaxUnconfirmedStarts +
                                              (starts >= KmdStages.MaxUnconfirmedStarts ? " (the guard now refuses to start)" : ""), level));
            p.Rows.Add(new Row("Confirm", Confirm(state, s, starts)));
            state.SetPanel(p);
        }

        // ADR 0006 point 3, as refined by E06: a start is good when the driver sits at "first present done" and
        // the desktop has stayed up for ConfirmAfterSeconds since we first saw that. Every start gets its own
        // confirmation: once per boot turned out too strict, a disable/enable or a re-install inside one boot
        // stayed unconfirmed and the next device cycle would have run into the guard.
        int _pendingStarts = -1, _pendingSince;

        string Confirm(State state, KmdSnapshot s, int starts)
        {
            if (starts == 0) { _pendingStarts = -1; return "nothing to confirm"; }
            if (!s.LastStage.HasValue || s.LastStage.Value != KmdStages.FirstPresentDone)
            {
                _pendingStarts = -1;
                return "waiting for stage " + KmdStages.FirstPresentDone;
            }
            if (_pendingStarts != starts) { _pendingStarts = starts; _pendingSince = Environment.TickCount; }
            int up = Math.Min(UptimeSeconds, unchecked(Environment.TickCount - _pendingSince) / 1000);
            if (up < ConfirmAfterSeconds)
                return "waiting, good for " + up + " s of " + ConfirmAfterSeconds;

            try
            {
                _kmd.Confirm();
                _pendingStarts = -1;
                state.Log(Name, Level.Good, "start confirmed: UnconfirmedStarts " + starts + " -> 0 (stage " +
                                            KmdStages.Describe(s.LastStage.Value) + ", good for " + up + " s)");
                return "done";
            }
            catch (Exception e)
            {
                if (_lastError != e.Message) state.Log(Name, Level.Error, "cannot confirm the start: " + e.Message);
                _lastError = e.Message;
                return "failed: " + e.Message;
            }
        }
    }
}
