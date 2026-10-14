// The ray tracing benchmark card on the Performance page (owner, 2026-10-11). Plain words only: the card says what
// the test does, where the game is, what it measured and what our own board reached. The rules are in
// src/Benchmark.cs and the run is in src/BenchmarkRun.cs, so this file only draws and reports.
//
// The card never searches the disk while a page is only being drawn for a gate (--smoke, --smoke-render): the
// search happens in WindowReading.Read, which skips it for those runs, and the fixture supplies a found copy for the render
// matrix. That keeps every build free of a file listing it has no business making.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Windows.Forms;

namespace AmdgpuWddmControl
{
    public sealed partial class MainForm
    {
        InstallState _bench;                    // where the game is; null: not searched (a gate run)
        BenchProgress _benchRun;                // the run that goes on, or the one that finished
        List<RunRecord> _benchHistory;
        string _benchNote;                      // the answer to the last folder choice, until the card is left

        // Where the game is, and what this computer measured before, come with the window's reading
        // (WindowReading.Read). A gate run (--smoke, --smoke-render of this PC, --smoke-perf) searches nothing: a
        // build must list no folder of the PC that builds it, and the render matrix gets a found copy from the
        // fixture instead.

        Control BuildBenchmarkCard(int width)
        {
            var c = new CardPanel(Strings.T("bench.title"), width);
            Mark("perf.benchmark", c);
            c.Add(Ui.Dim(Strings.T("bench.intro"), c.Inner));
            c.Add(Ui.Dim(Strings.T("bench.two-ways"), c.Inner));

            var state = _bench ?? new InstallState();
            bool busy = BenchRunner.Busy;
            if (state.Ready)
            {
                c.Pair(Strings.T("bench.game"), state.Folder);
                c.Pair(Strings.T("bench.size"), Strings.T("bench.size.value", BenchPlan.Width, BenchPlan.Height));
            }
            else
            {
                c.Add(Ui.Label(Strings.T(VerdictId(state.Verdict)), Theme.Bold, Theme.Warn, c.Inner));
                if (state.Verdict != InstallVerdict.NotFound && state.Folder != null) c.Pair(Strings.T("bench.game"), state.Folder);
            }
            if (_benchNote != null) c.Add(Ui.Dim(_benchNote, c.Inner));

            var buttons = Ui.WrapRow(c.Inner);
            Mark("perf.benchmark.buttons", buttons);
            if (state.Ready)
            {
                var start = Ui.Button(Strings.T("bench.start"), (s, e) => StartBenchmark(), true);
                start.Enabled = !busy && !_smoke;
                buttons.Controls.Add(start);
                var stop = Ui.Button(Strings.T("bench.cancel"), (s, e) => BenchRunner.Cancel());
                stop.Enabled = busy;
                buttons.Controls.Add(stop);
            }
            else if (state.Verdict == InstallVerdict.NotFound)
            {
                c.Add(Ui.Dim(Strings.T("bench.game.get"), c.Inner));
                buttons.Controls.Add(Ui.Button(Strings.T("bench.open.steam"), (s, e) => OpenBenchLink(Q2Rtx.StoreUrl)));
                buttons.Controls.Add(Ui.Button(Strings.T("bench.open.nvidia"), (s, e) => OpenBenchLink(Q2Rtx.ReleasesUrl)));
            }
            var choose = Ui.Button(Strings.T(state.Verdict == InstallVerdict.NotFound ? "bench.choose" : "bench.choose.again"), (s, e) => ChooseBenchmarkFolder());
            choose.Enabled = !busy && !_smoke;
            buttons.Controls.Add(choose);
            buttons.Controls.Add(Explain("benchmark", Strings.T("bench.title")));
            c.Add(buttons);

            var shown = ShownRun();
            var stateLine = Ui.Label(BenchStateText(), Theme.Bold, busy ? Theme.Teal : Theme.Dim, c.Inner);
            stateLine.AccessibleRole = AccessibleRole.StaticText;
            _live["bench.state"] = stateLine;
            c.Add(stateLine);

            if (shown != null)
            {
                foreach (var api in BenchPlan.Passes)
                {
                    var pass = shown.Pass(api);
                    if (pass == null) continue;
                    var name = Ui.Label(Strings.T("bench.pass." + BenchPlan.ApiName(api)), Theme.Bold, null, c.Inner);
                    c.Add(name);
                    c.Add(Ui.Label(Strings.T("bench.pass." + BenchPlan.ApiName(api) + ".note"), Theme.Small, Theme.Dim, c.Inner));
                    c.Pair(Strings.T("bench.result"), PassText(pass));
                    c.Pair(Strings.T("bench.temp.max"), pass.MaxTempC != null ? Strings.T("bench.temp.value", pass.MaxTempC.Value) : Strings.T("bench.notavailable"));
                    c.Pair(Strings.T("bench.clock.mean"), pass.MeanClockMHz != null ? Strings.T("bench.clock.value", pass.MeanClockMHz.Value) : Strings.T("bench.notavailable"));
                }
                c.Pair(Strings.T("bench.when"), BenchWhen(shown.Utc));
                c.Pair(Strings.T("bench.driver"), string.IsNullOrEmpty(shown.Driver) ? Strings.T("bench.notavailable") : shown.Driver);
            }

            // Our own board's numbers, only on a board the app has positively identified as a BC-250. The band and
            // the line under it say why the number of a run is never exactly the one before (b23 lab round 2, F6).
            if (UmaSetting.Visible(_uma))
            {
                c.Add(Ui.Dim(Strings.T("bench.reference", BenchText.Reference(BenchReference.LowFps), BenchText.Reference(BenchReference.HighFps),
                    BenchReference.Width, BenchReference.Height, BenchReference.Release), c.Inner));
                c.Add(Ui.Dim(Strings.T("bench.reference.why"), c.Inner));
            }

            // The runs before the one above: the card shows the newest saved run in full, so repeating it here would
            // show the same run twice (b23 lab round, F4).
            var history = BenchHistory.Earlier(_benchHistory, shown);
            c.Add(Ui.Label(Strings.T("bench.history"), Theme.Bold, null, c.Inner));
            if (history.Count == 0) c.Add(Ui.Dim(Strings.T("bench.history.none"), c.Inner));
            foreach (var r in history.Take(BenchHistory.Keep))
                c.Add(Ui.Dim(Strings.T("bench.history.line", BenchWhen(r.Utc), PassText(r.Pass(RtApi.Pipeline)), PassText(r.Pass(RtApi.Query))), c.Inner));
            c.Add(Ui.Dim(Strings.T(BenchTempRead ? "bench.note" : "bench.note.notemp"), c.Inner));
            return c;
        }

        // Whether the card may promise the thermal stop. The run watches the one temperature this page shows, so
        // where the page has none the stop has nothing to act on and only the time bound of each way is left. The
        // card then says that instead of a promise it cannot keep.
        bool BenchTempRead { get { return _snap != null && _snap.Dpm != null && _snap.Dpm.Has(DpmState.FlagTemperature); } }

        static string VerdictId(InstallVerdict verdict)
        {
            switch (verdict)
            {
                case InstallVerdict.NoFolder: return "bench.bad.folder";
                case InstallVerdict.NoProgram: return "bench.bad.program";
                case InstallVerdict.NoData: return "bench.bad.data";
                case InstallVerdict.NoDemo: return "bench.bad.demo";
                default: return "bench.game.none";
            }
        }

        // One pass in words: the measurement, or the plain reason there is none.
        static string PassText(PassRecord pass)
        {
            if (pass == null) return Strings.T("bench.notavailable");
            if (pass.Outcome == PassOutcome.Done && pass.Fps != null)
                return Strings.T("bench.result.fps", BenchText.Fps(pass.Fps.Value)) +
                    (pass.Frames != null && pass.Seconds != null
                        ? "  (" + Strings.T("bench.result.detail", pass.Frames.Value, BenchText.Seconds(pass.Seconds.Value)) + ")" : "");
            switch (pass.Outcome)
            {
                case PassOutcome.TooHot: return Strings.T("bench.result.hot");
                case PassOutcome.TimedOut: return Strings.T("bench.result.timeout");
                case PassOutcome.Cancelled: return Strings.T("bench.result.cancelled");
                case PassOutcome.NoResult: return Strings.T("bench.result.none");
                default: return Strings.T("bench.result.failed");
            }
        }

        static string BenchWhen(string utc)
        {
            var t = Recovery.Utc(utc);
            return t == null ? Strings.T("bench.notavailable") : When(t.Value);
        }

        // The run the card shows in full: the one of this session while there is one, otherwise the newest run saved
        // on this computer. The state line and the "Earlier runs" list follow it.
        RunRecord ShownRun()
        {
            if (_benchRun != null && _benchRun.Run != null) return _benchRun.Run;
            return _benchHistory != null ? _benchHistory.FirstOrDefault() : null;
        }

        string BenchStateText()
        {
            var p = _benchRun;
            string stage = p != null ? p.Stage : null, pass = p != null ? p.Pass : null;
            string id = BenchState.Id(stage, pass, ShownRun(), p != null && p.Run != null);
            if (id == BenchState.Running)
                return Strings.T(id, Strings.T("bench.pass." + pass), (p.ElapsedMs / 1000).ToString(CultureInfo.CurrentCulture));
            return Strings.T(id);
        }

        void StartBenchmark()
        {
            if (_smoke || _bench == null || !_bench.Ready) return;
            if (!BenchRunner.Start(_bench, InstalledVersion, OnBenchmarkProgress))
            {
                Result(Strings.T("bench.busy"), false);
                return;
            }
            _benchNote = null;
            _work = GuideCause.Repairing;                  // a job the person started: rank 4 of the guide
            _benchRun = BenchRunner.Progress;
            ComputeStatus();
            ShowPage(_page, null, false);
            // The buttons of the run the person has just started come into reach, wherever the page stood: "Stop the
            // test" is the one control that must be there while a run goes on (b23 lab round, F1). A page that shows
            // them already does not move at all, and every later rebuild keeps the card where it is (F7), so the
            // buttons stay under the pointer from Start to the end of the run.
            BringIntoView("perf.benchmark", "perf.benchmark.buttons");
        }

        // On the worker thread: marshal, then redraw the card only when something other than the clock changed.
        void OnBenchmarkProgress(BenchProgress p)
        {
            try
            {
                BeginInvoke((Action)(() =>
                {
                    if (IsDisposed) return;
                    string before = _benchRun == null ? null : _benchRun.Stage + "|" + _benchRun.Pass + "|" + PassCount(_benchRun);
                    _benchRun = p;
                    if (!p.Busy) { _work = null; _benchHistory = BenchPaths.ReadHistory(); ComputeStatus(); }
                    string now = p.Stage + "|" + p.Pass + "|" + PassCount(p);
                    Label live;
                    if (now == before && _live.TryGetValue("bench.state", out live)) live.Text = BenchStateText();
                    else if (_page == "performance") ShowPage(_page, null, false);
                }));
            }
            catch (InvalidOperationException) { }
        }

        static int PassCount(BenchProgress p) { return p == null || p.Run == null ? 0 : p.Run.Passes.Count; }

        void ChooseBenchmarkFolder()
        {
            if (_smoke || BenchRunner.Busy) return;
            string folder;
            using (var dialog = new FolderBrowserDialog { Description = Strings.T("bench.choose.title"), ShowNewFolderButton = false })
            {
                if (dialog.ShowDialog(this) != DialogResult.OK) return;
                folder = dialog.SelectedPath;
            }
            var state = Q2Rtx.Inspect(folder, new LocalBenchFiles());
            if (state.Ready)
            {
                _prefs.BenchmarkFolder = folder;
                _bench = state;
                _benchNote = null;
            }
            else
            {
                _bench = state;
                _benchNote = Strings.T(VerdictId(state.Verdict));
            }
            ShowPage(_page, null, false);
        }

        // The two pages of the game, and nothing else (Q2Rtx.AllowedLink).
        static void OpenBenchLink(string url)
        {
            if (!Q2Rtx.AllowedLink(url)) return;
            try { using (System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(url) { UseShellExecute = true })) { } }
            catch (Exception) { }
        }
    }
}
