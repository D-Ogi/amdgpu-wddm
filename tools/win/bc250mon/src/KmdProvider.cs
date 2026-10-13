// Startup records and guard confirmation through the shared state client.
// This reader performs no GPU call and remains usable when the adapter has Code 43.
// Full WDDM confirmation still requires the typed presentation witness below.
using System;
using System.Globalization;
using System.Diagnostics;
using System.ComponentModel;
using System.IO;

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
        public const int FirstPresentDone = 61;             // StageFirstPresentDone: display-only confirmation prerequisite
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
        public bool Installed;
        public bool Started; // A startup breadcrumb exists, not merely an initialized store.
        public int? LastStage, UnconfirmedStarts, StartStatus;
        public string History, StartResult, StartStep, StartDetail;
        public string Error;
    }

    public sealed class KmdRegistry
    {
        public const string DefaultPath = "bc250kmd shared service state";
        public readonly string Path = DefaultPath;
        public KmdRegistry(string overridePath)
        {
            if (!string.IsNullOrEmpty(overridePath))
                throw new ArgumentException("KMD registry overrides are unsupported. Tests use an injected state client.");
        }
        public KmdSnapshot Read()
        {
            var s = new KmdSnapshot();
            try
            {
                using (var state = DriverState.Open())
                {
                    s.Installed = true;
                    s.LastStage = (int?)state.Read("", "LastStage", false).Require();
                    s.UnconfirmedStarts = (int?)state.Read("", "UnconfirmedStarts", false).Require();
                    s.History = (string)state.Read("", "StageHistory", false).Require();
                    s.StartResult = (string)state.Read("StartRecord", "Result", false).Require();
                    s.StartStep = (string)state.Read("StartRecord", "Step", false).Require();
                    s.StartStatus = (int?)state.Read("StartRecord", "Status", false).Require();
                    s.StartDetail = (string)state.Read("StartRecord", "Detail", false).Require();
                    s.Started = s.LastStage.HasValue || s.History != null || s.StartResult != null;
                }
            }
            catch (Win32Exception e)
            {
                if (e.NativeErrorCode == 1060) s.Installed = false;
                else s.Error = e.Message;
            }
            catch (Exception e) { s.Error = e.Message; }
            return s;
        }
        /// <summary>Persist explicit confirmation. The native store checks its flush.</summary>
        public void Confirm()
        {
            using (var state = DriverState.Open(true))
                state.WriteDword("", "UnconfirmedStarts", 0);
        }

        public string Summary()
        {
            var s = Read();
            if (s.Error != null) return "bc250kmd state unreadable: " + s.Error;
            if (!s.Installed) return "bc250kmd not installed";
            if (!s.Started) return "bc250kmd installed, no startup record";
            return string.Format(CultureInfo.InvariantCulture, "bc250kmd UnconfirmedStarts={0} of {1}, LastStage={2}, history {3}",
                s.UnconfirmedStarts.HasValue ? s.UnconfirmedStarts.Value.ToString(CultureInfo.InvariantCulture) : "-",
                KmdStages.MaxUnconfirmedStarts,
                s.LastStage.HasValue ? KmdStages.Describe(s.LastStage.Value) : "-",
                KmdStages.Tail(s.History, 12));
        }
    }

    public sealed class KmdProvider : IProvider
    {
        // Full WDDM observes completed presentation; display-only retains stage61.
        public const int ConfirmAfterSeconds = 60;

        readonly KmdRegistry _kmd;
        readonly int _startTick = Environment.TickCount;
        readonly Stopwatch _healthClock = Stopwatch.StartNew();
        readonly StartConfirmationPolicy _healthPolicy = new StartConfirmationPolicy();
        string _lastHealthError;
        bool _retryFullConfirmation;
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

            if (!s.Installed && s.Error == null)
            {
                ResetAutoConfirmation();
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

            p.Rows.Add(new Row("Driver", s.Installed ? "installed" : "state unavailable", s.Installed ? Level.Good : Level.Error));
            AddKeyRow(p);
            if (s.Error != null)
            {
                ResetAutoConfirmation();
                p.Rows.Add(new Row("Key", s.Error, Level.Error));
                if (_lastError != s.Error) state.Log(Name, Level.Error, "cannot read " + _kmd.Path + ": " + s.Error);
                _lastError = s.Error;
                state.SetPanel(p);
                return;
            }
            _lastError = null;

            if (!s.Started)
            {
                ResetAutoConfirmation();
                p.Rows.Add(new Row("Stage", "no startup record"));
                state.SetPanel(p);
                return;
            }

            p.Rows.Add(new Row("Stage", s.LastStage.HasValue ? KmdStages.Describe(s.LastStage.Value) : "-",
                               s.LastStage.HasValue && (s.LastStage.Value == 90 || s.LastStage.Value == 91) ? Level.Error : Level.Info));
            p.Rows.Add(new Row("History", KmdStages.Tail(s.History, 12)));
            if (s.StartResult != null) {
                bool failed = s.StartResult == "failed" || s.StartResult == "guard";
                p.Rows.Add(new Row("Last start", s.StartResult + " / " + (s.StartStep ?? "-") +
                    (s.StartStatus.HasValue ? " / 0x" + s.StartStatus.Value.ToString("X8") : ""), failed ? Level.Error : Level.Info));
                if (failed && !string.IsNullOrEmpty(s.StartDetail)) p.Rows.Add(new Row("Reason", s.StartDetail, Level.Error));
            }

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

        void ResetAutoConfirmation()
        {
            _healthPolicy.Reset();
            _pendingStarts = -1;
        }

        string Confirm(State state, KmdSnapshot s, int starts)
        {
            // A failed checked flush can leave a cached registry zero. Keep the
            // full-WDDM retry pending until a typed confirmed reply says success.
            if (starts == 0 && !_retryFullConfirmation)
            { ResetAutoConfirmation(); return "nothing to confirm"; }
            try
            {
                StartHealthSnapshot health = Driver.ReadStartHealth();
                _lastHealthError = null;
                if ((health.Flags & 1) == 0)
                {
                    _healthPolicy.Reset();
                    _retryFullConfirmation = false;
                    // Only an affirmative, validated DDO reply permits the legacy
                    // stage61 policy. An old DLL/ABI or missing query never does.
                    return ConfirmDisplayOnly(state, s, starts);
                }
                _pendingStarts = -1;
                if ((health.Flags & 8) != 0)
                {
                    _healthPolicy.Reset(); _retryFullConfirmation = false;
                    _lastHealthError = null;
                    return "confirmed by driver";
                }
                if (!_healthPolicy.Observe(_healthClock.ElapsedMilliseconds, health))
                    return "waiting for presentation progress, " + (_healthPolicy.ObservedMilliseconds / 1000) + " s of 60";
                ulong generation = _healthPolicy.Generation, epoch = _healthPolicy.Epoch;
                _retryFullConfirmation = true;
                StartHealthSnapshot confirmed = Driver.ConfirmStartHealth(generation, epoch);
                _retryFullConfirmation = false;
                _healthPolicy.Reset(); _lastHealthError = null;
                state.Log(Name, Level.Good, "full-WDDM start confirmed after 60 s observed progress: generation " +
                    confirmed.Generation + ", epoch " + confirmed.Epoch + ", completed " + confirmed.Completed);
                return "confirmed after presentation progress";
            }
            catch (Exception e)
            {
                ResetAutoConfirmation();
                if (_lastHealthError != e.Message) state.Log(Name, Level.Warn, "automatic start confirmation waiting: " + e.Message);
                _lastHealthError = e.Message;
                return "waiting: " + e.Message;
            }
        }

        string ConfirmDisplayOnly(State state, KmdSnapshot s, int starts)
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
